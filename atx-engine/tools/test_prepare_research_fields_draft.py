"""prepare_research_fields_draft.py and research_fields_v9.py (lane FIELDS-V9, ruling PM5-13): the draft fields v13
list is off by default and moves nothing of the v8 field lists (fields v9 to v12), of any existing field's formula or
of any existing producer fingerprint, so the reuse counts of the v8 builds cannot move; promoting the module with the
two-line builder hook would not move them either. Synthetic sources only (no stage is read).
"""
import json
from pathlib import Path
import unittest

import prepare_research_fields as tool
import prepare_research_fields_draft as draft
import research_fields_holdings as hold
import research_fields_price as price
import research_fields_sec as sec
import research_fields_v8 as v8
import research_fields_v9 as v9
from test_research_fields_v9_nt import drafted

REPO = Path(__file__).resolve().parents[2]
V9_SPECS = ("scripts/specs/v71.json", "scripts/specs/v8/base-lo1.json", "scripts/specs/v8/base-lo3.json")
EXISTING_MODULES = (sec, price, v8, hold)
HOOK = b"\nimport research_fields_v9 as _v9  # noqa: E402\nFIELD_MODULES.append(_v9.bind(globals()))\n"


def lf(blob: bytes) -> bytes:
    return blob.replace(b"\r\n", b"\n")


def fingerprints(host: bytes) -> dict:
    """Every producer fingerprint --reuse keys on: the builder's field groups and each existing module's groups."""
    out = {"builder": tool.producer_fingerprints(host)}
    for m in EXISTING_MODULES:
        out[m.__name__] = tool.module_fingerprints(m, tool.module_source(m), host)
    return out


def edited(source: bytes, head: bytes, new: bytes) -> bytes:
    if source.count(head) != 1:
        raise AssertionError(f"edit anchor {head!r} is not unique")
    return source.replace(head, new)


class DraftIsOffByDefault(unittest.TestCase):
    def test_the_plain_builder_does_not_know_the_draft(self):
        self.assertEqual(draft.FIELDS_V13_DRAFT, tuple(v9.FIELDS))
        self.assertEqual((draft.DRAFT_VERSION, draft.DRAFT_MODULES), ("v13", (v9,)))
        for name in draft.FIELDS_V13_DRAFT:
            self.assertNotIn(name, tool.ALL_FIELDS)
            self.assertNotIn(name, hold.HOLD_FIELDS)
            self.assertNotIn(name, tool.DEFAULT_FIELDS)
        self.assertFalse(any(isinstance(m, v9.V9FieldModule) for m in tool.FIELD_MODULES))
        for m in (tool,) + EXISTING_MODULES:      # no existing source names the draft module
            self.assertNotIn(b"research_fields_v9", Path(m.__file__).read_bytes(), m.__name__)
        # fields v9 (the 63-field list of the v8 base cells); v10 to v12 add existing registry fields only
        for rel in V9_SPECS:
            names = json.loads((REPO / rel).read_text(encoding="utf-8"))["fields"]["list"]
            self.assertEqual(len(names), 63, rel)
            self.assertFalse(set(names) & set(draft.FIELDS_V13_DRAFT), rel)
            self.assertTrue(set(names) <= set(tool.ALL_FIELDS) | set(hold.HOLD_FIELDS), rel)

    def test_registration_appends_only_the_draft_fields(self):
        plain, specs = list(tool.ALL_FIELDS), dict(tool.ALL_FIELDS)
        fids = {x: tool.formula_id(x, tool.spec_definition(x, 1)) for x in plain}
        modules, defaults = list(tool.FIELD_MODULES), tool.DEFAULT_FIELDS
        with drafted():
            self.assertEqual(list(tool.ALL_FIELDS), plain + list(draft.FIELDS_V13_DRAFT))
            for x in plain:
                self.assertIs(tool.ALL_FIELDS[x], specs[x], x)
                self.assertEqual(tool.formula_id(x, tool.spec_definition(x, 1)), fids[x], x)
            self.assertEqual(tool.FIELD_MODULES[:-1], modules)
            self.assertIsInstance(tool.FIELD_MODULES[-1], v9.V9FieldModule)
            self.assertEqual(draft.register(vars(tool)), [tool.FIELD_MODULES[-1]])   # registering twice adds nothing
            self.assertEqual(len(tool.FIELD_MODULES), len(modules) + 1)
            self.assertEqual(tool.DEFAULT_FIELDS, defaults)
            for name in draft.FIELDS_V13_DRAFT:
                self.assertTrue(v9.FIELDS[name]["point_in_time"])
                self.assertEqual(v9.producer_group(name), v9.FIELDS[name]["group"])
            self.assertEqual({v9.producer_group(x) for x in v9.FIELDS}, set(v9.PRODUCERS))
            tool.module_reuse_interface(v9)
        self.assertEqual(list(tool.ALL_FIELDS), plain)                 # restored for every other test module
        self.assertEqual(tool.FIELD_MODULES, modules)
        with self.assertRaisesRegex(ValueError, "already registered"):
            v9.bind({"ALL_FIELDS": {"nt_first_126": dict(v9.FIELDS["nt_first_126"])}})

    def test_existing_fingerprints_are_unmoved_by_the_draft_and_its_promotion(self):
        host = lf(tool.builder_source())
        base = fingerprints(host)
        for group, fps in base.items():
            self.assertTrue(fps and all(isinstance(v, str) and len(v) == 64 for v in fps.values()), group)
        with drafted():
            self.assertEqual(fingerprints(lf(tool.builder_source())), base)     # registration edits no source
        self.assertEqual(fingerprints(host + HOOK), base)                       # nor would the builder hook
        # teeth: a module-level ALL_FIELDS.update in the builder joins the host closure of the modules that read
        # spec_definition through their handle (why bind registers the fields itself)
        moved = fingerprints(host + b"\nALL_FIELDS.update(_v9.FIELDS)\n")
        self.assertNotEqual(moved[sec.__name__], base[sec.__name__])
        self.assertEqual(moved["builder"], base["builder"])
        # the draft module's own groups follow their own closure only
        fps = tool.module_fingerprints(v9, tool.module_source(v9), host)
        self.assertEqual(sorted(fps), sorted(v9.PRODUCERS))
        src = lf(tool.module_source(v9))
        nt_edit = edited(src, b"def first_flags(cidx: np.ndarray, avail: np.ndarray, lookback_ns: int) -> np.ndarray:",
                         b"def first_flags(cidx: np.ndarray, avail: np.ndarray, lookback_ns: int, _e=None):")
        changed = tool.module_fingerprints(v9, nt_edit, host)
        self.assertEqual({k for k in fps if fps[k] != changed[k]}, {"v9_nt"})
        earn_edit = edited(src, b"def average_ranks(x: np.ndarray) -> np.ndarray:",
                           b"def average_ranks(x: np.ndarray, _e=None) -> np.ndarray:")
        changed = tool.module_fingerprints(v9, earn_edit, host)
        self.assertEqual({k for k in fps if fps[k] != changed[k]}, {"v9_earnseason"})

    def test_imported_code_pins_follow_the_imported_closure(self):
        host = lf(tool.builder_source())
        v8src, secsrc = lf(Path(v8.__file__).read_bytes()), lf(Path(sec.__file__).read_bytes())
        earn = v9.imported_code("v9_earnseason")
        nt = v9.imported_code("v9_nt")
        self.assertEqual((earn["module"], nt["module"]), ("research_fields_v8.py", "research_fields_sec.py"))
        same = lambda group, **kw: v9.imported_code(group, **kw)["sha256"]
        # research_fields_v8: an imported helper moves the pin; another producer of that module does not
        self.assertNotEqual(same("v9_earnseason", source=edited(
            v8src, b"def gathered(values: np.ndarray, rows: np.ndarray) -> np.ndarray:",
            b"def gathered(values: np.ndarray, rows: np.ndarray, _e=None) -> np.ndarray:")), earn["sha256"])
        self.assertEqual(same("v9_earnseason", source=edited(
            v8src, b"def gscore_rows(h, hist: dict, role, output: Path, budget, lag: int) -> dict:",
            b"def gscore_rows(h, hist: dict, role, output: Path, budget, lag: int, _e=None) -> dict:")), earn["sha256"])
        # the builder code the imported names read through ``h`` (LatestRows -> h.advance) moves it; other code not
        self.assertNotEqual(same("v9_earnseason", builder=edited(
            host, b"def advance(ev: dict, latest: np.ndarray, p: int, mark: int) -> int:",
            b"def advance(ev: dict, latest: np.ndarray, p: int, mark: int, _e=None) -> int:")), earn["sha256"])
        self.assertEqual(same("v9_earnseason", builder=edited(
            host, b"def reuse_fields(prior_dir: Path,", b"def reuse_fields(prior_dir: Path | None,")), earn["sha256"])
        # research_fields_sec: the calendar rule (reached from Calendar) moves the nt pin; the Form 4 reader does not
        self.assertNotEqual(same("v9_nt", source=edited(
            secsrc, b"def nyse_holidays(year: int) -> set:", b"def nyse_holidays(year: int, _e=None) -> set:")),
            nt["sha256"])
        self.assertEqual(same("v9_nt", source=edited(
            secsrc, b"    def _insider(self, options, ciks, cal: Calendar, end_ns: int, budget):",
            b"    def _insider(self, options, ciks, cal: Calendar, end_ns: int, budget, _e=None):")), nt["sha256"])
        self.assertEqual(same("v9_nt", source=secsrc.replace(b"class Windowed:", b"class Windowed:  # a comment")),
                         nt["sha256"])


if __name__ == "__main__":
    unittest.main()
