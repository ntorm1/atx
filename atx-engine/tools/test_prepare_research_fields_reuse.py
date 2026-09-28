"""--reuse (platform v7 L2): copy unchanged field payloads from a prior fields directory, compute only the rest.

Synthetic analogue of the root acceptance (fields-v6b -> fields-v7): build A = the 40-field recipe (legacy +
issuer), then B = A's list + sv_ratio126 with --reuse A. The 40 payloads must be byte-identical to A, marked
reused, and sv_ratio126 computed; B must equal a fresh build C of the same 41 fields in every payload byte and
every manifest entry (apart from the reuse records). No real archive access.
"""
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest

import prepare_research_fields as tool
import test_prepare_research_fields as base
import test_prepare_research_fields_sv as svt

ISSUER_RUN = base.ISSUER_RUN                     # the 40-field recipe of the byte-identity test
SMALL = ["si_shares", "si_dtc", "iv_atm_21d", "shares_out", "mkt_ret"]


def strip_reuse(manifest):
    m = json.loads(json.dumps(manifest))
    m.pop("reuse", None)
    for e in m["fields"]:
        e.pop("reused_from", None)
    return m


class Env:
    def __init__(self, root: Path):
        self.fx = svt.SvFixture(root)
        self.bridge, self.events = root / "bridge", root / "events"
        self.bridge_sha, self.events_sha = base.write_bridge(self.bridge), base.write_events(self.events)

    def produce(self, out, fields, **kw):
        kw.setdefault("identity_bridge", self.bridge)
        kw.setdefault("identity_bridge_sha256", self.bridge_sha)
        kw.setdefault("fund_events", self.events)
        kw.setdefault("fund_events_sha256", self.events_sha)
        return self.fx.run(out, fields=list(fields), **kw)

    def path(self, out, name="manifest.json"):
        return self.fx.base / out / name


class FieldReuse(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.env = Env(Path(cls.temp.name))
        cls.a = cls.env.produce("A", ISSUER_RUN)
        cls.a_sha = base.sha(cls.env.path("A"))
        cls.b = cls.env.produce("B", ISSUER_RUN + ["sv_ratio126"], reuse=cls.env.path("A").parent,
                                reuse_sha256=cls.a_sha)
        cls.c = cls.env.produce("C", ISSUER_RUN + ["sv_ratio126"])

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_n_payloads_byte_identical_to_a_and_marked_reused(self):
        self.assertEqual(len(ISSUER_RUN), 40)
        block = self.b["reuse"]
        self.assertEqual(block["reused"], ISSUER_RUN)
        self.assertEqual(block["computed"], ["sv_ratio126"])
        self.assertEqual(block["not_reused"], {"sv_ratio126": "absent from the prior manifest"})
        self.assertEqual(block["manifest_sha256"], self.a_sha)
        self.assertEqual(block["mode"], "copy")
        a_entries = {e["name"]: e for e in self.a["fields"]}
        for e in self.b["fields"]:
            name = e["name"]
            if name == "sv_ratio126":
                self.assertNotIn("reused_from", e)
                continue
            self.assertEqual(base.sha(self.env.path("B", f"{name}.f64")), base.sha(self.env.path("A", f"{name}.f64")))
            r = e["reused_from"]
            self.assertEqual((r["manifest_sha256"], r["payload_sha256"], r["mode"]),
                             (self.a_sha, self.a["files"][f"{name}.f64"]["sha256"], "copy"))
            self.assertEqual(r["formula_id"], tool.formula_id(name, tool.spec_definition(name, 1)))
            self.assertEqual(len(r["inputs_sha256"]), 64)
            e = dict(e)
            e.pop("reused_from")
            self.assertEqual(e, a_entries[name])  # the prior entry verbatim

    def test_new_field_computed_and_b_equals_a_fresh_build(self):
        self.assertEqual([e["name"] for e in self.b["fields"]], ISSUER_RUN + ["sv_ratio126"])
        self.assertEqual(self.b["files"], self.c["files"])
        for name in self.c["files"]:
            self.assertEqual(base.sha(self.env.path("B", name)), base.sha(self.env.path("C", name)), name)
        self.assertEqual(strip_reuse(self.b), self.c)          # source_checks carried from A == recomputed in C
        self.assertEqual(self.b["reuse"]["source_checks_from_prior"],
                         ["issuer", "si_dtc", "si_shares", "tickerhistory"])

    def test_without_reuse_the_manifest_is_unchanged(self):
        for m in (self.a, self.c):
            self.assertNotIn("reuse", m)
            self.assertFalse(any("reused_from" in e for e in m["fields"]))

    def test_refusals(self):
        a_dir = self.env.path("A").parent
        with self.assertRaisesRegex(ValueError, "--reuse-sha256"):
            self.env.produce("R1", SMALL, reuse=a_dir, reuse_sha256="0" * 64)
        bad = self.env.fx.base / "A-bad"
        shutil.copytree(a_dir, bad)
        blob = bytearray((bad / "iv_atm_21d.f64").read_bytes())
        blob[8] ^= 1
        (bad / "iv_atm_21d.f64").write_bytes(bytes(blob))
        with self.assertRaisesRegex(ValueError, "corrupt prior dir"):
            self.env.produce("R2", SMALL, reuse=bad)
        other = self.env.fx.base / "A-other-role"
        shutil.copytree(a_dir, other)
        m = json.loads((other / "manifest.json").read_text(encoding="utf-8"))
        m["role"]["manifest_sha256"] = "1" * 64
        (other / "manifest.json").write_text(json.dumps(m), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "different role"):
            self.env.produce("R3", SMALL, reuse=other)
        self.assertFalse((self.env.fx.base / "R2" / "manifest.json").exists())  # nothing published on refusal

    def test_hardlink_mode_and_cli(self):
        a_dir = self.env.path("A").parent
        h = self.env.produce("H", SMALL, reuse=a_dir, reuse_hardlink=True)
        self.assertEqual(h["reuse"]["mode"], "hardlink")
        for name in SMALL:
            self.assertEqual(base.sha(self.env.path("H", f"{name}.f64")), base.sha(self.env.path("A", f"{name}.f64")))
        self.assertGreaterEqual(os.stat(self.env.path("H", "iv_atm_21d.f64")).st_nlink, 2)
        fx = self.env.fx
        with contextlib.redirect_stdout(io.StringIO()):
            tool.main(["--role", str(fx.role), "--role-sha256", fx.role_sha, "--output", str(fx.base / "cli"),
                       "--fields", "si_shares,iv_atm_21d,mkt_ret", "--finra", str(fx.finra), "--tickerhistory",
                       str(fx.th), "--lake", str(fx.lake), "--reuse", str(a_dir), "--reuse-sha256", self.a_sha])
        m = json.loads((fx.base / "cli" / "manifest.json").read_bytes())
        self.assertEqual(m["reuse"]["reused"], ["si_shares", "iv_atm_21d", "mkt_ret"])
        self.assertEqual(m["reuse"]["computed"], [])


class ReuseInvalidation(unittest.TestCase):
    """Changed inputs, changed formula revision and dependencies force recomputation (own fixture: it mutates)."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.env = Env(Path(self.temp.name))
        self.a = self.env.produce("A", SMALL)
        self.a_dir = self.env.path("A").parent

    def tearDown(self):
        self.temp.cleanup()

    def test_changed_source_recomputes_the_field_and_its_dependents(self):
        fx = self.env.fx
        rows = base.FINRA_SHARES + [(303, "2024-10-10", "777")]
        shutil.rmtree(fx.finra)
        base.write_finra(fx.finra, shares_rows=rows)
        b = self.env.produce("B", SMALL, reuse=self.a_dir)
        c = self.env.produce("C", SMALL)
        blk = b["reuse"]
        self.assertEqual(blk["reused"], ["iv_atm_21d", "mkt_ret"])
        self.assertEqual(blk["computed"], ["si_shares", "si_dtc", "shares_out"])
        self.assertIn("changed", blk["not_reused"]["si_shares"])
        self.assertIn("changed", blk["not_reused"]["si_dtc"])            # the asof manifest it pins changed too
        self.assertTrue(blk["not_reused"]["shares_out"].startswith("depends on si_shares"))
        self.assertNotEqual(base.sha(self.env.path("B", "si_shares.f64")), base.sha(self.env.path("A", "si_shares.f64")))
        for name in c["files"]:
            self.assertEqual(base.sha(self.env.path("B", name)), base.sha(self.env.path("C", name)), name)
        # every entry equals the fresh build; only the partially reused group's source check (tickerhistory: its
        # counters cover the recomputed fields only) differs, and the prior one is kept in the reuse block
        sb = strip_reuse(b)
        self.assertEqual(sb["fields"], c["fields"])
        self.assertEqual(sb["source_checks"].pop("tickerhistory") != c["source_checks"].pop("tickerhistory"), True)
        self.assertEqual({k: v for k, v in sb.items() if k != "source_checks"},
                         {k: v for k, v in c.items() if k != "source_checks"})
        self.assertEqual(sb["source_checks"], c["source_checks"])
        self.assertEqual(blk["prior_source_checks_of_partial_groups"],
                         {"tickerhistory": self.a["source_checks"]["tickerhistory"]})

    def test_formula_revision_bump_and_moved_source_are_not_reused(self):
        tool.FORMULA_REVISION["iv_atm_21d"] = 2
        try:
            b = self.env.produce("B", SMALL, reuse=self.a_dir)
        finally:
            del tool.FORMULA_REVISION["iv_atm_21d"]
        self.assertEqual(b["reuse"]["not_reused"], {"iv_atm_21d": "formula id differs (definition or FORMULA_REVISION "
                                                                  "changed)"})
        self.assertEqual(b["formula_revisions"], {"iv_atm_21d": 2})
        self.assertNotIn("formula_revisions", self.a)
        # back at revision 1, B's revision-2 payload is not reused either (the revision is part of the formula id)
        b2 = self.env.produce("B2", SMALL, reuse=self.env.path("B").parent)
        self.assertEqual(list(b2["reuse"]["not_reused"]), ["iv_atm_21d"])
        fx = self.env.fx
        moved = fx.base / "finra-moved"
        shutil.copytree(fx.finra, moved)
        d = self.env.produce("D", SMALL, reuse=self.a_dir, finra=moved)   # same bytes, another source argument
        self.assertIn("outside this run's inputs", d["reuse"]["not_reused"]["si_shares"])
        self.assertEqual(d["reuse"]["computed"], ["si_shares", "si_dtc", "shares_out"])


if __name__ == "__main__":
    unittest.main()
