"""--reuse of the field modules (platform v8 C-3): research_fields_sec.py and research_fields_holdings.py fields are
copied from a prior fields directory by the module's own PRODUCERS closure (plus the builder code it reads through its
host handle), its stage manifest pins and its formula. Synthetic stages only (the SEC, holdings and sv fixtures).
"""
import gzip
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import prepare_research_fields as tool
import research_fields_holdings as hold
import research_fields_sec as sec
import test_prepare_research_fields as base
import test_prepare_research_fields_sec as sect
import test_prepare_research_fields_sv as svt
import test_research_fields_holdings as holdt

# the 63-field recipe: the default and issuer fields, sv_ratio126, every SEC field and every holdings field
RECIPE = list(tool.DEFAULT_FIELDS) + list(tool.ISSUER_FIELDS) + ["sv_ratio126"] + list(sec.FIELDS) + list(hold.HOLD_FIELDS)
HOLD_WRITERS = (("thirteenf", holdt.write_thirteenf), ("ftd", holdt.write_ftd), ("regsho_threshold", holdt.write_regsho),
                ("security_master", holdt.write_security_master), ("short_volume_ext", holdt.write_svx))


class ModuleFixture(svt.SvFixture):
    """The sv fixture plus the issuer inputs, the SEC stages and bridge, and the holdings stages."""

    def __init__(self, root: Path):
        super().__init__(root)
        self.sec = sect.SecFixture.__new__(sect.SecFixture)
        self.sec.stages = root / "alpha_panel"
        self.sec.pins = {
            "earnings_calendar_sha256": sect.write_stage(self.sec.stages / "earnings_calendar",
                                                         sec.STAGES["earnings_calendar"][1],
                                                         {"announcements.parquet": sect.announcements_table()}),
            "insider_sha256": sect.write_stage(self.sec.stages / "insider", sec.STAGES["insider"][1],
                                               sect.insider_tables()),
            "sec_filings_sha256": sect.write_stage(self.sec.stages / "sec_filings", sec.STAGES["sec_filings"][1],
                                                   {"eight_k_items.parquet": sect.eightk_table()})}
        self.sec.sec_bridge = root / "sec_bridge"
        self.sec.sec_bridge_sha = base.write_bridge(self.sec.sec_bridge, rows=sect.SEC_BRIDGE)
        self.hold = {}
        for key, writer in HOLD_WRITERS:
            self.hold[key] = root / key
            self.hold[key + "_sha256"] = writer(root / key)
        bridge, events = root / "bridge", root / "events"
        self.issuer = dict(identity_bridge=bridge, identity_bridge_sha256=base.write_bridge(bridge),
                           fund_events=events, fund_events_sha256=base.write_events(events))

    def run(self, out, **kw):
        kw.setdefault("module_options", sect.SecFixture.options(self.sec))
        for k, v in {**self.hold, **self.issuer}.items():
            kw.setdefault(k, v)
        return super().run(out, **kw)


def entry(manifest, name):
    return next(e for e in manifest["fields"] if e["name"] == name)


class ModuleReuse(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.fx = ModuleFixture(Path(cls.temp.name))
        cls.full = cls.fx.run("full", fields=RECIPE)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_reuse_self_reports_63_reused(self):
        self.assertEqual(len(RECIPE), 63)
        self.assertEqual([e["name"] for e in self.full["fields"]], RECIPE)
        again = self.fx.run("again", fields=RECIPE, reuse=self.fx.base / "full")
        block = again["reuse"]
        self.assertEqual(block["reused"], RECIPE)
        self.assertEqual((len(block["reused"]), block["computed"], block["not_reused"]), (63, [], {}))
        self.assertEqual(again["files"], self.full["files"])
        for name in RECIPE:
            self.assertEqual((self.fx.base / "again" / f"{name}.f64").read_bytes(),
                             (self.fx.base / "full" / f"{name}.f64").read_bytes(), name)
            e = dict(entry(again, name))
            rec = e.pop("reused_from")
            self.assertEqual(e, entry(self.full, name), name)
            self.assertEqual(rec["payload_sha256"], self.full["files"][f"{name}.f64"]["sha256"], name)
        for name in list(sec.FIELDS) + list(hold.HOLD_FIELDS):   # module fields name their module producer
            rec = entry(again, name)["reused_from"]
            module = "research_fields_sec.py" if name in sec.FIELDS else "research_fields_holdings.py"
            self.assertEqual(rec["producer"]["module"], module, name)
            self.assertEqual(rec["host_code"]["code_sha256_lf"], self.full["code_sha256_lf"], name)
            self.assertEqual(rec["inputs"], (sec if name in sec.FIELDS else hold).entry_inputs(entry(self.full, name)))
        # the source checks of fully reused groups are the prior's; holdings keeps this run's stage pins
        self.assertEqual(again["source_checks"]["sec"], self.full["source_checks"]["sec"])
        for key in hold.KIND_CHECK_KEY.values():
            self.assertEqual(again["source_checks"]["holdings"][key], self.full["source_checks"]["holdings"][key], key)
        self.assertEqual(again["source_checks"]["holdings"]["code"], self.full["source_checks"]["holdings"]["code"])
        self.assertIn("sec", block["source_checks_from_prior"])
        self.assertIn("holdings.ftd", block["source_checks_from_prior"])
        # a reuse of the reuse still copies everything (chained reused_from keeps the origin's producer)
        third = self.fx.run("third", fields=RECIPE, reuse=self.fx.base / "again")
        self.assertEqual((len(third["reuse"]["reused"]), third["files"]), (63, self.full["files"]))

    def test_one_producer_edit_recomputes_one_field(self):
        path = Path(hold.__file__)
        original = path.read_bytes().replace(b"\r\n", b"\n")
        head = b"def build_ftd(ctx: Ctx, names, stage: Stage):"
        self.assertEqual(original.count(head), 1)
        edited = original.replace(head, b"def build_ftd(ctx: Ctx, names, stage: Stage, _edited=None):")
        ident = tool.code_identity_of(original)
        blobs = {ident["code_git_blob_sha1"]: original}   # git's object store holds the unedited module
        real_source, real_blob = tool.module_source, tool.git_blob

        def source(module):
            return edited if module is hold else real_source(module)

        with mock.patch.object(tool, "module_source", source), \
                mock.patch.object(tool, "git_blob", lambda sha1: blobs.get(sha1) or real_blob(sha1)):
            again = self.fx.run("edited", fields=RECIPE, reuse=self.fx.base / "full")
        block = again["reuse"]
        self.assertEqual(block["computed"], ["ftd_shares_ratio21"])
        self.assertEqual(block["reused"], [x for x in RECIPE if x != "ftd_shares_ratio21"])
        self.assertIn("producing code differs", block["not_reused"]["ftd_shares_ratio21"])
        self.assertIn("group ftd", block["not_reused"]["ftd_shares_ratio21"])
        self.assertEqual(again["files"], self.full["files"])   # the recomputed field has the same bytes
        e = entry(again, "ftd_shares_ratio21")
        self.assertNotIn("reused_from", e)
        self.assertEqual(e["producer"]["code_sha256_lf"], tool.code_identity_of(edited)["code_sha256_lf"])
        for name in ("inst_own_share", "regsho_threshold_days63", "sv_offexchange_share126"):
            self.assertEqual(entry(again, name)["reused_from"]["producer_code"].split(";")[0],
                             "research_fields_holdings.py: git blob " + ident["code_git_blob_sha1"], name)
        # this run's ftd check covers the recomputed field; the other kinds carry the prior's
        self.assertEqual(again["source_checks"]["holdings"]["ftd"], self.full["source_checks"]["holdings"]["ftd"])
        self.assertNotIn("holdings.ftd", block["source_checks_from_prior"])
        self.assertIn("holdings.thirteenf", block["source_checks_from_prior"])

    def test_stage_pin_change_recomputes_its_kind(self):
        """A holdings kind whose stage manifest pin changed is recomputed; the SEC fields follow their own pins."""
        fx = self.fx
        prior = json.loads((fx.base / "full" / "manifest.json").read_text(encoding="utf-8"))
        e = entry(prior, "regsho_threshold_days63")
        self.assertEqual(sorted(e["stage_manifest_sha256"]), ["regsho_threshold", "security_master"])
        options = {k: v for k, v in fx.hold.items()}
        options["ftd_sha256"] = "0" * 64
        got = hold.reuse_inputs("ftd_shares_ratio21", options)
        self.assertNotEqual(got, hold.entry_inputs(entry(prior, "ftd_shares_ratio21")))
        self.assertEqual(hold.reuse_inputs("regsho_threshold_days63", options), e["stage_manifest_sha256"])
        s = entry(prior, "k8_count_63")
        self.assertEqual(sec.reuse_inputs("k8_count_63", sect.SecFixture.options(fx.sec)), sec.entry_inputs(s))
        self.assertNotEqual(sec.reuse_inputs("k8_count_63", sect.SecFixture.options(fx.sec, sec_filings_sha256="0" * 64)),
                            sec.entry_inputs(s))


class SvDirectorySource(unittest.TestCase):
    """sv_ratio126's CNMS directory source (REUSE_DIRECTORY_RULE): reused while the files it reads re-hash to the
    recorded list; a changed file in the read set recomputes it."""

    def test_sv_directory_reuse_and_change(self):
        with tempfile.TemporaryDirectory() as temp:
            fx = svt.SvFixture(Path(temp))
            run = ["si_shares", "shares_out", "sv_ratio126"]
            a = fx.run("a", fields=run)
            b = fx.run("b", fields=run, reuse=fx.base / "a")
            self.assertEqual((b["reuse"]["reused"], b["reuse"]["computed"]), (run, []))
            self.assertEqual(b["reuse"]["directory_rule"], tool.REUSE_DIRECTORY_RULE)
            self.assertEqual(b["files"], a["files"])
            src = next(x for x in entry(a, "sv_ratio126")["sources"] if "files_sha256" in x)
            first = "CNMSshvol" + src["first_file_date"].replace("-", "") + ".txt.gz"
            path = fx.sv / first
            raw = gzip.decompress(path.read_bytes())
            path.write_bytes(gzip.compress(raw, mtime=1))   # same rows, other .gz bytes: the recorded list differs
            c = fx.run("c", fields=run, reuse=fx.base / "a")
            self.assertEqual(c["reuse"]["reused"], ["si_shares", "shares_out"])
            self.assertIn("CNMS files this run reads differ", c["reuse"]["not_reused"]["sv_ratio126"])
            self.assertEqual(c["files"]["sv_ratio126.f64"], a["files"]["sv_ratio126.f64"])
            # a reuse that checks no directory source keeps the pre-C-3 block keys
            d = fx.run("d", fields=run[:2], reuse=fx.base / "a")
            self.assertNotIn("directory_rule", d["reuse"])
            self.assertNotIn("module_rule", d["reuse"])


class ModuleFingerprints(unittest.TestCase):
    def test_module_groups_follow_their_own_closure(self):
        src = tool.module_source(hold)
        fps = tool.module_fingerprints(hold, src, tool.builder_source())
        self.assertEqual(sorted(fps), sorted(hold.PRODUCERS))
        self.assertTrue(all(isinstance(v, str) and len(v) == 64 for v in fps.values()))
        # a comment or docstring does not count; an edit in build_ftd changes only the ftd group
        lf = src.replace(b"\r\n", b"\n")
        self.assertEqual(tool.module_fingerprints(hold, lf.replace(b"def build_ftd(", b"# note\ndef build_ftd("),
                                                  tool.builder_source()), fps)
        edited = tool.module_fingerprints(hold, lf.replace(b"def build_ftd(ctx: Ctx, names, stage: Stage):",
                                                           b"def build_ftd(ctx: Ctx, names, stage: Stage, _e=None):"),
                                          tool.builder_source())
        self.assertEqual({k for k in fps if fps[k] != edited[k]}, {"ftd"})
        # a builder definition a module reads through its handle is part of every group that reads it, and only those
        host = tool.builder_source().replace(b"\r\n", b"\n")
        sec_src = tool.module_source(sec)
        sec_fp = tool.module_fingerprints(sec, sec_src, host)
        self.assertEqual(sorted(sec_fp), [sec.GROUP])
        head = b"def load_bridge(directory: Path, expected_sha256: str, role: Role, budget: Budget):"
        self.assertEqual(host.count(head), 1)
        edit = host.replace(head, head[:-2] + b", _edit=None):")          # read by the SEC module only
        self.assertNotEqual(tool.module_fingerprints(sec, sec_src, edit), sec_fp)
        self.assertEqual(tool.module_fingerprints(hold, src, edit), fps)
        head = b"class FieldWriter:"
        self.assertEqual(host.count(head), 1)
        edit = host.replace(head, b"class FieldWriter(object):")          # read by both modules
        self.assertNotEqual(tool.module_fingerprints(sec, sec_src, edit), sec_fp)
        changed = tool.module_fingerprints(hold, src, edit)
        self.assertTrue(all(changed[k] != fps[k] for k in fps))
        head = b"def reuse_fields(prior_dir: Path,"                      # read by neither
        self.assertEqual(host.count(head), 1)
        edit = host.replace(head, b"def reuse_fields(prior_dir: Path | None,")
        self.assertEqual(tool.module_fingerprints(sec, sec_src, edit), sec_fp)
        self.assertEqual(tool.module_fingerprints(hold, src, edit), fps)


if __name__ == "__main__":
    unittest.main()
