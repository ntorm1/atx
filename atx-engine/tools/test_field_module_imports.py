"""Every producer closure's cross-module names are pinned (finding FD-2, P9 lane A1).

``code_fingerprint.fingerprints`` hashes a producer's AST closure in its own source, so a name the closure reads from
another module of this directory (``from M import x``, or ``alias.x`` after ``import M as alias``) is pinned only by
the import statement, not by the code it names. Each such name must be in the module's declared imports, whose closure
``imported_code`` pins as an entry input (``IMPORTS``, ``PRICE_IMPORTS``, ``HOLD_IMPORTS``), or be one of
``DATA_PINS``: names whose effect --reuse pins as data.
"""
from __future__ import annotations

import importlib
from pathlib import Path
import unittest

import code_fingerprint as cf
import field_registry as fr
import prepare_research_fields as tool

TOOLS = Path(__file__).resolve().parent
LOCAL = frozenset(p.stem for p in TOOLS.glob("*.py"))
# (imported module, name or "*") -> how --reuse pins it instead of an imports closure
DATA_PINS = {
    ("research_window", "*"): "the research window: its seal is recorded in every manifest's seal block, which "
                              "load_prior refuses when it differs (P9 A1), and holdings entries pin seal_date; its "
                              "code is pinned by test_research_window.py and the gtest ResearchWindow.HeaderMatchesJson",
    ("research_fields_sec", "nyse_sessions"): "the rule calendar: an entry input (session_calendar, its digest) of "
                                              "every field whose group reads it",
}
CALENDAR_READERS = {"research_fields_price": ("price_open", "price_ceq", "price_coskew"),
                    "research_fields_gold": ("gold_cols",)}


def declared(module, group: str) -> dict:
    """{imported module name: set of names} the module declares for producer group ``group``."""
    out: dict = {}

    def add(mod, names):
        out.setdefault(mod.__name__, set()).update(names)

    imports = getattr(module, "IMPORTS", None)
    if isinstance(imports, dict):
        if group in imports:
            add(*imports[group])
    elif imports is not None:
        for row in imports:
            add(row[0], row[1])
    if imports is None and hasattr(module, "PRICE_IMPORTS"):
        add(module.price, module.PRICE_IMPORTS)
    if imports is None and hasattr(module, "HOLD_IMPORTS"):
        add(module.hold, module.HOLD_IMPORTS)
    return out


def modules() -> list:
    return [importlib.import_module(x) for x in fr.python_modules(fr.load())]


def undeclared(reads: dict, declares: dict) -> list:
    gaps = []
    for mod, names in reads.items():
        for name in names:
            if (mod, "*") in DATA_PINS or (mod, name) in DATA_PINS or name in declares.get(mod, set()):
                continue
            gaps.append(f"{mod}.{name}")
    return gaps


class ProducerImports(unittest.TestCase):
    def test_every_field_module_declares_what_its_producers_import(self):
        gaps = {}
        for module in modules():
            src = Path(module.__file__).read_bytes().replace(b"\r\n", b"\n")
            for group, entries in module.PRODUCERS.items():
                reads = cf.cross_module_reads(src, entries, LOCAL - {module.__name__})
                missing = undeclared(reads, declared(module, group))
                if missing:
                    gaps[f"{module.__name__}:{group}"] = missing
        self.assertEqual(gaps, {})

    def test_the_builders_own_producers_read_only_data_pinned_modules(self):
        src = tool.builder_source().replace(b"\r\n", b"\n")
        for group, entries in tool.FIELD_PRODUCERS.items():
            reads = cf.cross_module_reads(src, entries, LOCAL - {"prepare_research_fields"},
                                          tool.PRODUCER_ORCHESTRATION)
            self.assertEqual(undeclared(reads, {}), [], group)

    def test_declared_imports_are_what_imported_code_pins(self):
        """The declarations this test reads are the ones imported_code hashes (each names existing definitions)."""
        for module in modules():
            if not hasattr(module, "imported_code"):
                continue
            for group in module.PRODUCERS:
                for mod_name, names in declared(module, group).items():
                    src = (TOOLS / f"{mod_name}.py").read_bytes().replace(b"\r\n", b"\n")
                    self.assertTrue(cf.Module(src).has(sorted(names)), f"{module.__name__}:{group} {mod_name}")
                module.imported_code(group)   # raises when a declared name is missing

    def test_calendar_data_pin_is_an_entry_input_of_its_readers(self):
        for name, groups in CALENDAR_READERS.items():
            module = importlib.import_module(name)
            for group in groups:
                field = next(x for x in module.FIELDS if module.producer_group(x) == group)
                self.assertIn("session_calendar", module.reuse_inputs(field, {}), f"{name}:{group}")

    def test_the_check_has_teeth(self):
        src = (b"import research_fields_sec as sec\nfrom research_fields_v8 import picked\nfrom os import path\n"
               b"def produce(x):\n    return sec.Calendar(x) + picked(x) + path.sep\n"
               b"def other():\n    return sec.STAGES\n")
        reads = cf.cross_module_reads(src, ("produce",), {"research_fields_sec", "research_fields_v8"})
        self.assertEqual(reads, {"research_fields_sec": ["Calendar"], "research_fields_v8": ["picked"]})
        self.assertEqual(undeclared(reads, {"research_fields_sec": {"Calendar"}}), ["research_fields_v8.picked"])
        with self.assertRaisesRegex(ValueError, "lacks one of"):
            cf.cross_module_reads(src, ("absent",), set())


if __name__ == "__main__":
    unittest.main()
