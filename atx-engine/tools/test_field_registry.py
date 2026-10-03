"""The field registry (K-P9-1, field_registry.json) and the one builder entry ``--registry`` (P9 lane A1).

* the committed registry is the code's registry: generated from today's four registration mechanisms it reproduces the
  committed bytes (fresh interpreter, repository window: some SEC clocks embed the seal date), the same field list and
  order in this harness, and fields v15's 84 names in v15's manifest order;
* ``dtype`` is declared (classifier units), not inferred from a ``grp_`` prefix;
* the entry binds every module a row names, refuses a registry that disagrees with the code, expands ``--fields all``,
  and builds the same bytes as the plain builder; engine rows go to the executable only with ``--engine-exe``;
* the four shims are thin deprecated wrappers; holdings is a late FIELD_MODULES module (its fields last).
Synthetic data only (the slice-1 fixture's role and FINRA inputs).
"""
from __future__ import annotations

import contextlib
import copy
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import field_registry as fr
import prepare_research_fields as tool
import prepare_research_fields_draft as draft
import prepare_research_fields_ohlc as ohlc_shim
import prepare_research_fields_xdata as xdata_shim
import prepare_research_fields_ydata as ydata_shim
import prepare_research_fields_engine as engine
import research_fields_holdings as hold
from test_research_window import isolated

TOOLS = Path(__file__).resolve().parent
FIXTURE = TOOLS.parents[0] / "tests" / "fixtures" / "research_fields"
sys.path.insert(0, str(FIXTURE))
import make_research_fields_fixture as fx  # noqa: E402  (the slice-1 synthetic inputs)

SHIMS = (draft, xdata_shim, ohlc_shim, ydata_shim)
# fields v15 (build-equity/train-2020-2023-lo3-fields-v15/manifest.json, its "fields" names in manifest order; the
# field list is open under the blind rule): 84 names.
V15_FIELDS = (
    "si_shares", "si_dtc", "iv_atm_21d", "iv_atm_63d", "iv_atm_126d", "earn_recent", "shares_out", "mkt_ret", "be",
    "at", "at_lag4", "lt", "che", "debt", "sale_ttm", "gp_ttm", "oi_ttm", "ni_ttm", "ni_q", "ni_q_lag4", "be_lag1q",
    "be_lag1q_lag4", "cfo_ttm", "capx_ttm", "xrd_ttm", "dvc_ttm", "prstkc_ttm", "sstk_ttm", "txt_q", "txt_q_lag4",
    "shrs_q", "shrs_q_lag4", "noa", "noa_lag4", "sue", "fscore", "me_company", "grp_sic2", "grp_ff12", "grp_ff49",
    "sv_ratio126", "ea_days_to_expected", "ea_days_since", "ea_window_pre5", "ea_window_post3", "ea_delay_days",
    "ea_time_of_day", "ins_net_buy_ratio", "ins_n_buyers", "ins_n_sellers", "ins_opportunistic_net", "ins_cluster_buy",
    "k8_count_63", "k8_item_material_21", "k8_days_since_any", "ret_overnight", "ret_intraday", "ceq_iss_5y",
    "coskew_60m", "vol_126", "xrd0_ttm", "grp_ff12f49", "nt_first_126", "earn_season_rank", "div_month_pred",
    "beta_dvol_21", "season_y2_5", "open_adj", "high_adj", "low_adj", "iv_skew_21", "stio_chg_q", "div_init_omit",
    "deal_pending", "conn_ret63", "inst_own_share", "inst_breadth_chg", "inst_own_chg_q", "inst_best_ideas",
    "inst_n_holders", "ftd_shares_ratio21", "regsho_threshold_days63", "sv_offexchange_share126", "exch_up_365d")
GROUP_FIELDS = {"grp_sic2", "grp_ff12", "grp_ff49", "grp_ff12f49", "ea_time_of_day"}


@contextlib.contextmanager
def every_module():
    """The builder with every shim's modules bound (the base commit's fields v13+ one-liner); restored on exit."""
    with mock.patch.dict(tool.ALL_FIELDS), mock.patch.object(tool, "FIELD_MODULES", list(tool.FIELD_MODULES)):
        for shim in SHIMS:
            shim.register(vars(tool))
        yield


@contextlib.contextmanager
def plain_builder():
    """The builder exactly as imported (the entry binds modules into it; undone on exit)."""
    with mock.patch.dict(tool.ALL_FIELDS), mock.patch.object(tool, "FIELD_MODULES", list(tool.FIELD_MODULES)):
        yield


class CommittedRegistry(unittest.TestCase):
    def test_valid_and_in_v15_order(self):
        doc = fr.load()
        names = fr.names(doc)
        self.assertEqual(doc["schema"], "atx.field-registry/v1")
        self.assertEqual(len(names), 92)
        self.assertEqual(len(V15_FIELDS), 84)
        self.assertEqual([x for x in names if x in V15_FIELDS], list(V15_FIELDS))   # v15's manifest order
        self.assertTrue(all(set(row) == set(fr.ROW_KEYS) for row in doc["fields"]))
        # engine rows: only the ported fields the engine path routes, each named by its own BuilderKind id (lane A2's
        # P5 flip of si_shares, si_dtc, vol_126 keeps this green; every other row is python)
        flips = fr.engine_flips(doc)
        self.assertTrue(set(flips) <= set(engine.ENGINE_FIELDS), flips)
        self.assertTrue(all(kind_id == name for name, kind_id in flips.items()), flips)
        self.assertEqual(names[-len(hold.HOLD_FIELDS):], list(hold.HOLD_FIELDS))    # holdings last, as manifests
        self.assertEqual(fr.dump(doc), fr.DEFAULT_PATH.read_bytes())                # committed in dump's form

    def test_generated_from_the_four_mechanisms_equals_the_committed_file(self):
        """Fresh interpreter, repository window: ``generate`` reproduces every committed byte and the entry's check
        accepts the file (some SEC clock texts embed the seal date, so this harness's bound window cannot)."""
        code = ("import hashlib, importlib, json\n"
                "import prepare_research_fields as b, field_registry as fr\n"
                "for s in fr.SHIMS: importlib.import_module(s).register(vars(b))\n"
                "blob = fr.dump(fr.generate(vars(b), engine=fr.engine_flips(fr.load())))\n"
                "fr.check(vars(b), fr.load())\n"
                "print(json.dumps({'equal': blob == fr.DEFAULT_PATH.read_bytes(), 'rows': len(fr.load()['fields'])}))\n")
        got = isolated(TOOLS, code)
        self.assertEqual(got, {"equal": True, "rows": 92})

    def test_this_harness_generates_the_same_rows_up_to_window_dependent_text(self):
        doc = fr.load()
        with every_module():
            gen = fr.generate(vars(tool), engine=fr.engine_flips(doc))
        self.assertEqual(fr.names(gen), fr.names(doc))
        for a, b in zip(gen["fields"], doc["fields"]):
            self.assertEqual({k: v for k, v in a.items() if k not in ("spec_text", "formula_sha256")},
                             {k: v for k, v in b.items() if k not in ("spec_text", "formula_sha256")}, a["name"])

    def test_lane_a2_flip_keeps_the_registry_tests_green(self):
        """The P5 edit (si_shares, si_dtc, vol_126 -> kind engine, builder = own name; nothing else) is what
        ``generate(engine=engine_flips(file))`` reproduces, and the entry's check accepts it."""
        flipped = copy.deepcopy(fr.load())
        for row in flipped["fields"]:
            if row["name"] in ("si_shares", "si_dtc", "vol_126"):
                row.update(kind="engine", builder=row["name"])
        fr.validate(flipped)
        self.assertEqual(fr.engine_flips(flipped), {x: x for x in ("si_shares", "si_dtc", "vol_126")})
        with every_module():
            gen = fr.generate(vars(tool), engine=fr.engine_flips(flipped))
            fr.check(vars(tool), gen)
        for a, b in zip(gen["fields"], flipped["fields"]):
            self.assertEqual({k: v for k, v in a.items() if k not in ("spec_text", "formula_sha256")},
                             {k: v for k, v in b.items() if k not in ("spec_text", "formula_sha256")}, a["name"])
        with self.assertRaisesRegex(fr.RegistryError, "not producible"):
            fr.generate(vars(tool), engine={"hv_21d": "hv_21d"})

    def test_dtype_is_declared_by_units_not_by_name(self):
        doc = fr.load()
        group = {row["name"] for row in doc["fields"] if row["dtype"] == "group"}
        self.assertEqual(group, GROUP_FIELDS)
        prefix = {row["name"] for row in doc["fields"] if row["name"].startswith("grp_")}
        self.assertEqual(group - prefix, {"ea_time_of_day"})   # the classifier a grp_ inference reads as f64 (DS 2)
        self.assertEqual(prefix - group, set())

    def test_engine_ported_rows_carry_the_engines_formula(self):
        """si_shares, si_dtc, vol_126: kind python at A1 (A2's P5 flip makes them engine rows named by their own
        BuilderKind ids); either way the formula is engine.py's identity check and the owner names the engine shim."""
        doc = {row["name"]: row for row in fr.load()["fields"]}
        for name in engine.ENGINE_FIELDS:
            row = doc[name]
            self.assertIn((row["kind"], row["builder"]), (("python", row["builder"]), ("engine", name)))
            self.assertIn("engine-shim", row["owner"])
            self.assertEqual(row["formula_sha256"],
                             tool.formula_id(name, tool.spec_definition(name, engine.price.LAG_SESSIONS)))
        self.assertEqual((doc["si_shares"]["options"], doc["vol_126"]["options"]),
                         ({"group": "finra"}, {"group": "price_volume"}))


class Validation(unittest.TestCase):
    def doc(self):
        return copy.deepcopy(fr.load())

    def test_malformed_documents_are_refused(self):
        cases = []
        d = self.doc()
        del d["fields"][0]["owner"]
        cases.append((d, "exactly the keys"))
        d = self.doc()
        d["fields"][1]["name"] = d["fields"][0]["name"]
        cases.append((d, "new non-empty"))
        d = self.doc()
        d["fields"][0]["kind"] = "rust"
        cases.append((d, "kind in"))
        d = self.doc()
        d["fields"][0]["requires"] = ["exch_up_365d"]
        cases.append((d, "no earlier row"))
        d = self.doc()
        d["fields"][0]["formula_sha256"] = "ABC"
        cases.append((d, "hex"))
        d = self.doc()
        d["schema"] = "atx.field-registry/v0"
        cases.append((d, "not an atx.field-registry/v1"))
        for doc, why in cases:
            with self.assertRaisesRegex(fr.RegistryError, why):
                fr.validate(doc)

    def test_a_registry_that_disagrees_with_the_code_is_refused(self):
        with every_module():
            ns = vars(tool)
            good = fr.generate(ns)
            fr.check(ns, good)
            swapped = copy.deepcopy(good)
            swapped["fields"][0], swapped["fields"][1] = swapped["fields"][1], swapped["fields"][0]
            owner = copy.deepcopy(good)
            owner["fields"][0]["builder"] = "research_fields_price"
            formula = copy.deepcopy(good)
            formula["fields"][0]["formula_sha256"] = "0" * 64
            pit = copy.deepcopy(good)
            pit["fields"][0]["point_in_time"] = False
            dropped = copy.deepcopy(good)
            dropped["fields"].pop()
            for doc, why in ((swapped, "order"), (owner, "produces it"), (formula, "spec text"),
                             (pit, "point_in_time"), (dropped, "without a row: exch_up_365d")):
                with self.assertRaisesRegex(fr.RegistryError, why):
                    fr.check(ns, doc)
            engine_row = copy.deepcopy(good)   # an engine row names a BuilderKind id, not a module
            engine_row["fields"][0].update(kind="engine", builder="si_shares")
            fr.check(ns, engine_row)

    def test_select(self):
        doc = fr.load()
        self.assertEqual(fr.select(doc, "all"), fr.names(doc))
        self.assertEqual(fr.select(doc, " vol_126 ,si_shares"), ["vol_126", "si_shares"])
        with self.assertRaisesRegex(fr.RegistryError, "hv_21d not in the field registry"):
            fr.select(doc, "si_shares,hv_21d")


class Entry(unittest.TestCase):
    """The entry on the slice-1 synthetic inputs: same bytes as the plain builder."""

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.base = Path(cls.temp.name)
        cls.role_sha = fx.write_role(cls.base / "role")
        fx.write_finra(cls.base / "finra")
        with every_module():
            cls.registry = cls.base / "registry.json"   # this harness's window (the committed file is the repo's)
            cls.registry.write_bytes(fr.dump(fr.generate(vars(tool))))

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def argv(self, out, *extra):
        return ["--role", str(self.base / "role"), "--role-sha256", self.role_sha, "--output", str(self.base / out),
                "--finra", str(self.base / "finra"), *extra]

    def manifest(self, out):
        return json.loads((self.base / out / "manifest.json").read_bytes())

    def same(self, a, b):
        ma, mb = self.manifest(a), self.manifest(b)
        self.assertEqual(json.dumps(ma, sort_keys=True).replace(str(self.base / a), "<out>"),
                         json.dumps(mb, sort_keys=True).replace(str(self.base / b), "<out>"))
        for f in ma["files"]:
            self.assertEqual((self.base / a / f).read_bytes(), (self.base / b / f).read_bytes(), f)

    def test_entry_builds_the_plain_builders_bytes(self):
        fields = "vol_126,si_shares,si_dtc"
        with plain_builder(), contextlib.redirect_stdout(io.StringIO()):
            tool.main(self.argv("plain", "--fields", fields))
            tool.main(self.argv("entry", "--fields", fields, "--registry", str(self.registry)))
            modules = {type(m).__module__ for m in tool.FIELD_MODULES}
        self.same("plain", "entry")
        self.assertEqual([e["name"] for e in self.manifest("entry")["fields"]], ["si_shares", "si_dtc", "vol_126"])
        self.assertTrue({"research_fields_v9", "research_fields_connected"} <= modules)   # every row's module bound

    def test_engine_rows_need_the_executable(self):
        doc = fr.load(self.registry)
        for row in doc["fields"]:
            if row["name"] in ("si_dtc", "vol_126", "div_month_pred", "beta_dvol_21", "season_y2_5"):
                row.update(kind="engine", builder=row["name"])   # lane A2's listed edit (and a later A3-style flip)
        flipped = self.base / "flipped.json"
        flipped.write_bytes(fr.dump(doc))
        # an engine row's builder is a kind id: its Python fallback module comes from the owner, never from builder
        self.assertNotIn("vol_126", fr.python_modules(doc))
        self.assertIn("research_fields_xdata", fr.python_modules(doc))   # every xdata row flipped: owner's shim
        err = io.StringIO()
        with plain_builder(), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
            tool.main(self.argv("python-fallback", "--fields", "si_dtc,vol_126", "--registry", str(flipped)))
            tool.main(self.argv("plain-dtc", "--fields", "si_dtc,vol_126"))
            self.assertIn("research_fields_xdata", {type(m).__module__ for m in tool.FIELD_MODULES})
        self.assertIn("engine rows si_dtc, vol_126 are computed by the Python builder", err.getvalue())
        self.same("plain-dtc", "python-fallback")
        with plain_builder(), self.assertRaisesRegex(engine.EngineError, "is not a file"):
            tool.main(self.argv("engine", "--fields", "si_dtc", "--registry", str(flipped),
                                "--engine-exe", str(self.base / "no-such.exe")))
        with plain_builder(), self.assertRaisesRegex(fr.RegistryError, "no requested row is of kind engine"):
            tool.main(self.argv("no-engine", "--fields", "si_shares", "--registry", str(flipped),
                                "--engine-exe", str(self.base / "no-such.exe")))

    def test_the_script_entry_reproduces_the_identity_fixture(self):
        """``python prepare_research_fields.py --registry field_registry.json`` (the committed registry, a fresh
        interpreter in the repository window, the script re-entering its importable instance) builds the slice-1
        fixture's committed payloads byte for byte, and the same manifest as the plain script."""
        def script(out, *extra):
            done = subprocess.run([sys.executable, "prepare_research_fields.py", *self.argv(out, *extra)], cwd=TOOLS,
                                  capture_output=True, text=True, timeout=600)
            self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        names = ",".join(fx.FIELDS)
        script("script-plain", "--fields", names)
        script("script-entry", "--registry", str(fr.DEFAULT_PATH), "--fields", names)
        self.same("script-plain", "script-entry")
        for name in fx.FIELDS:
            self.assertEqual((self.base / "script-entry" / f"{name}.f64").read_bytes(),
                             (FIXTURE / "expected" / f"{name}.f64").read_bytes(), name)

    def test_entry_refuses_a_stale_registry_before_any_output(self):
        doc = fr.load(self.registry)
        doc["fields"][0]["formula_sha256"] = "0" * 64
        stale = self.base / "stale.json"
        stale.write_bytes(fr.dump(doc))
        with plain_builder(), self.assertRaisesRegex(fr.RegistryError, "formula_sha256 differs"):
            tool.main(self.argv("stale", "--fields", "all", "--registry", str(stale)))
        self.assertFalse((self.base / "stale").exists())


class Registration(unittest.TestCase):
    def test_shims_are_thin_wrappers_over_the_registry_binder(self):
        for shim in SHIMS:
            src = Path(shim.__file__).read_text(encoding="utf-8")
            self.assertIn("DEPRECATED", src)
            self.assertIn("field_registry.bind_modules(host_namespace, DRAFT_MODULES)", src)
            self.assertNotIn("present = [m for m in modules", src)   # the copied body is gone
        with plain_builder():
            first = draft.register(vars(tool))
            self.assertEqual(draft.register(vars(tool)), first)        # idempotent
            self.assertEqual(fr.bind_modules(vars(tool), draft.DRAFT_MODULES), first)

    def test_holdings_is_a_late_field_module(self):
        late = fr.late_modules(vars(tool))
        self.assertEqual([type(m).__module__ for m in late], ["research_fields_holdings"])
        self.assertTrue(all(x not in tool.ALL_FIELDS for x in hold.HOLD_FIELDS))
        self.assertFalse(hasattr(hold, "register"))
        self.assertEqual(list(fr.code_fields(vars(tool)))[-len(hold.HOLD_FIELDS):], list(hold.HOLD_FIELDS))

    def test_run_takes_module_options_as_keywords_and_refuses_others(self):
        with self.assertRaisesRegex(TypeError, "unexpected keyword arguments: thirteenf_pin"):
            tool.run(Path("no-role"), "0" * 64, Path("no-out"), ["si_shares"], thirteenf_pin="x")
        self.assertTrue(set(hold.STAGE_KWARGS) <= {k for m in tool.FIELD_MODULES for k in m.OPTIONS})


if __name__ == "__main__":
    unittest.main()
