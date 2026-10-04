"""The research-store class registry guard (P9 SQL2; sql-design section 3.9, synthetic).

Every ``atx.<name>/v<n>`` literal in non-test Python under ``scripts/``, ``atx-engine/tools/``, ``atx-impl/tools/`` and
in C++ under ``atx-engine/include/``, ``atx-engine/src/``, ``atx-impl/src/`` must name a file class of
``atx-engine/schemas/research_store/classes.json`` (a class's ``schemas`` or ``registers``), a digest / document
domain (``domains``) or a dated ``legacy_allow`` row that has not expired. A new writer schema therefore lands with
its class row (or an explicit, dated exception), and the catalog never meets an unclassified document by surprise.
The scan reads file text (comments and docstrings included); test files (``test_*.py``, ``*_test.py``,
``conftest.py``) and anything under a ``tests`` or ``fixtures`` directory are skipped.

Run: python -m pytest -q -p no:cacheprovider atx-engine/tools/test_research_store_classes.py
"""
from __future__ import annotations

import copy
import datetime
import json
import re
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
REGISTRY = REPO / "atx-engine" / "schemas" / "research_store" / "classes.json"
LITERAL = re.compile(r"atx\.[a-z0-9][a-z0-9_.-]*/v[0-9]+")
PY_DIRS = ("scripts", "atx-engine/tools", "atx-impl/tools")
CPP_DIRS = ("atx-engine/include", "atx-engine/src", "atx-impl/src")
CPP_SUFFIXES = frozenset({".hpp", ".cpp", ".h", ".ipp", ".inl"})
SKIP_DIRS = frozenset({"tests", "fixtures", "__pycache__"})


def is_test_file(rel: str) -> bool:
    parts = rel.split("/")
    name = parts[-1]
    return (name.startswith("test_") or name.endswith("_test.py") or name == "conftest.py"
            or any(p in SKIP_DIRS for p in parts[:-1]))


def scanned_files(repo: Path) -> list:
    files = []
    for d in PY_DIRS:
        files += [p for p in (repo / d).rglob("*.py") if not is_test_file(p.relative_to(repo).as_posix())]
    for d in CPP_DIRS:
        files += [p for p in (repo / d).rglob("*") if p.suffix in CPP_SUFFIXES and p.is_file()
                  and not is_test_file(p.relative_to(repo).as_posix())]
    return sorted(files)


def scan(repo: Path) -> dict:
    """{literal: sorted repo-relative files naming it}."""
    found: dict = {}
    for path in scanned_files(repo):
        for m in LITERAL.finditer(path.read_text(encoding="utf-8", errors="replace")):
            found.setdefault(m.group(0), set()).add(path.relative_to(repo).as_posix())
    return {k: sorted(v) for k, v in sorted(found.items())}


def registered(registry: dict, today: datetime.date) -> set:
    """Class schemas and registers, domains, and the legacy_allow ids whose expiry is not past ``today``."""
    out = set()
    for c in registry["classes"]:
        out.update(c.get("schemas", []))
        out.update(c.get("registers", []))
    out.update(d["id"] for d in registry.get("domains", []))
    out.update(a["id"] for a in registry.get("legacy_allow", [])
               if datetime.date.fromisoformat(a["expires"]) >= today)
    return out


def unregistered(found: dict, registry: dict, today: datetime.date) -> dict:
    known = registered(registry, today)
    return {literal: files for literal, files in found.items() if literal not in known}


def load(path: Path = REGISTRY) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


WILDCARD = re.compile(r"[*?\[]")


def shared_literal_problems(registry: dict) -> list:
    """Why the classes sharing a schema literal could shadow one another ([] when they cannot).

    One writer may stamp one schema on two file names (run_bounded_research.py: start.json and receipt.json). Such a
    literal may sit in several classes only when every glob of every such class ends in a literal file name (no
    ``*``, ``?`` or ``[`` in its last segment) and no file name (lower-cased: the catalog folds case) belongs to two
    of them. Then no path matches two of those classes, so the first-match rule never makes a later one dead.
    """
    owners: dict = {}
    for c in registry["classes"]:
        for literal in [*c["schemas"], *c.get("registers", [])]:
            owners.setdefault(literal, []).append(c)
    problems = []
    for literal, classes in sorted(owners.items()):
        if len(classes) < 2:
            continue
        holder: dict = {}
        for c in classes:
            if not c["globs"]:
                problems.append(f"{literal}: class {c['id']} has no globs")
            for glob in c["globs"]:
                name = glob.rsplit("/", 1)[-1].lower()
                if WILDCARD.search(name):
                    problems.append(f"{literal}: class {c['id']} glob {glob!r} names no literal file")
                elif holder.setdefault(name, c["id"]) != c["id"]:
                    problems.append(f"{literal}: file name {name!r} in classes {holder[name]} and {c['id']}")
    return problems


class ClassRegistryGuard(unittest.TestCase):
    def test_every_schema_literal_is_registered(self):
        missing = unregistered(scan(REPO), load(), datetime.date.today())
        self.assertEqual(missing, {}, "unregistered schema literals: add the schema to its class in "
                                      f"{REGISTRY.relative_to(REPO).as_posix()} (or a dated legacy_allow row): "
                                      + "; ".join(f"{k} in {', '.join(v[:3])}" for k, v in missing.items()))

    def test_legacy_allow_rows_are_dated_reasoned_and_not_classes(self):
        registry = load()
        classes = set()
        for c in registry["classes"]:
            classes.update(c.get("schemas", []))
            classes.update(c.get("registers", []))
        ids = [a["id"] for a in registry["legacy_allow"]]
        self.assertEqual(len(ids), len(set(ids)))
        today = datetime.date.today()
        for row in registry["legacy_allow"]:
            with self.subTest(id=row["id"]):
                self.assertRegex(row["id"], f"^{LITERAL.pattern}$")
                self.assertTrue(row.get("reason"))
                self.assertGreaterEqual(datetime.date.fromisoformat(row["expires"]), today,
                                        "an expired legacy_allow row: register the class or drop the literal")
                self.assertNotIn(row["id"], classes)
        for row in registry["domains"]:
            self.assertRegex(row["id"], f"^{LITERAL.pattern}$")
            self.assertTrue(row.get("reason"))

    def test_registry_shape(self):
        registry = load()
        self.assertEqual(registry["schema"], "atx.research-store-classes/v1")
        ids = [c["id"] for c in registry["classes"]]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertEqual(registry["classes"][-1]["id"], "other")
        self.assertEqual(registry["classes"][-1]["globs"], ["**"])
        for c in registry["classes"]:
            with self.subTest(cls=c["id"]):
                for key in ("id", "label", "globs", "schemas", "format", "ingest", "render", "writer", "pinned_by",
                            "stage"):
                    self.assertIn(key, c)
                self.assertIn(c["format"], ("json", "jsonl", "bytes"))
                for literal in [*c["schemas"], *c.get("registers", [])]:
                    self.assertRegex(literal, f"^{LITERAL.pattern}$")
        self.assertEqual(shared_literal_problems(registry), [])

    def test_shared_literal_needs_disjoint_literal_file_names(self):
        # Fix round 1 (S1): the review's probes, each a class sharing atx.bounded-research-run/v1 placed ahead of
        # run-receipt, and more; every one could shadow run-receipt or run-start and must be reported.
        registry = load()
        literal = "atx.bounded-research-run/v1"
        self.assertEqual(sorted(c["id"] for c in registry["classes"] if literal in c["schemas"]),
                         ["run-receipt", "run-start"])
        at = next(i for i, c in enumerate(registry["classes"]) if c["id"] == "run-receipt")

        def with_probe(globs: list) -> dict:
            planted = copy.deepcopy(registry)
            planted["classes"].insert(at, {"id": "probe", "globs": globs, "schemas": [literal]})
            return planted

        for globs in (["build-equity/**"], ["**/*receipt.json"], ["**/*"], ["**/*.json"], ["**"],
                      ["**/receipt.json"], ["build-equity/x/START.json"], ["**/rec?ipt.json"], ["**/[rs]tart.json"],
                      ["**/finish.json", "**/*.json"], []):
            with self.subTest(globs=globs):
                self.assertNotEqual(shared_literal_problems(with_probe(globs)), [])
        # A third class with its own literal file name cannot shadow either: allowed.
        self.assertEqual(shared_literal_problems(with_probe(["**/finish.json"])), [])
        # Unshared literals are not constrained (catch-all globs are the norm there).
        self.assertEqual(shared_literal_problems({"classes": [{"id": "a", "globs": ["**/*.json"],
                                                               "schemas": ["atx.a/v1"]}]}), [])

    def test_planted_unregistered_literal_fails(self):
        registry = load()
        today = datetime.date(2026, 10, 3)
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            planted = {
                "scripts/new_writer.py": 'SCHEMA = "atx.planted-writer/v1"\n',
                "atx-engine/tools/sub/deep.py": "# writes atx.planted-deep/v2\n",
                "atx-impl/src/new_exe.cpp": 'constexpr auto kSchema = "atx.planted-exe/v3";\n',
                "atx-engine/include/atx/x.hpp": '// atx.bounded-research-run/v1 (registered)\n',
                # skipped: tests, fixtures, other suffixes, other dirs
                "scripts/test_new_writer.py": '"atx.planted-test/v1"\n',
                "atx-impl/tools/tests/helper.py": '"atx.planted-test/v2"\n',
                "atx-engine/src/fixtures/f.cpp": '"atx.planted-test/v3"\n',
                "scripts/notes.md": "atx.planted-doc/v1\n",
                "atx-db/src/x.py": '"atx.planted-elsewhere/v1"\n',
            }
            for rel, text in planted.items():
                path = repo / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
            found = scan(repo)
            self.assertEqual(sorted(found), ["atx.bounded-research-run/v1", "atx.planted-deep/v2",
                                             "atx.planted-exe/v3", "atx.planted-writer/v1"])
            missing = unregistered(found, registry, today)
            self.assertEqual(missing, {"atx.planted-deep/v2": ["atx-engine/tools/sub/deep.py"],
                                       "atx.planted-exe/v3": ["atx-impl/src/new_exe.cpp"],
                                       "atx.planted-writer/v1": ["scripts/new_writer.py"]})
            # A legacy_allow row covers a literal until its expiry, and no longer.
            registry["legacy_allow"].append({"id": "atx.planted-writer/v1", "reason": "test", "expires": "2026-10-03"})
            self.assertNotIn("atx.planted-writer/v1", unregistered(found, registry, today))
            self.assertIn("atx.planted-writer/v1", unregistered(found, registry, today + datetime.timedelta(days=1)))


if __name__ == "__main__":
    unittest.main()
