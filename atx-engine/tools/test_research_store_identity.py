"""research_store_identity: every render rule against bytes the real writers produce (P9 SQL2, synthetic).

Each case writes files in a temp tree with the writer's own call (``stage_chain.Chain.write``, ``wave_queue.write``,
``cycle_verdict.keep_copy``, ``cycle_resume.write_binding``, ``wave_context.Wave.write_json``,
``backtest_integrity.ledger_append``) or, where the writer is a script's ``main``, the exact write expression of that
writer (cited); inserts the rows the catalog's ingest would write through the generic accessor
(``research_store.Store``) into a catalog made by SQL1's test-only ``make_store.py`` from the committed group fixtures;
and checks that the checker re-renders the same bytes. CRLF and LF files both; a one-byte edit is reported. The last
case runs the C++ catalog (``atx-research-store``) over the committed fixture tree when the executable is built.
Nothing here is real data.

Run: python -m pytest -q -p no:cacheprovider atx-engine/tools/test_research_store_identity.py
"""
from __future__ import annotations

import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

import research_store as rs
import research_store_identity as rsi

REPO = Path(__file__).resolve().parents[2]
FIXTURES = REPO / "atx-engine" / "tests" / "fixtures" / "research_store"
sys.path.insert(0, str(FIXTURES))
sys.path.append(str(REPO / "scripts"))
sys.path.append(str(REPO / "atx-impl" / "tools"))
import backtest_integrity  # noqa: E402
import cycle_resume  # noqa: E402
import cycle_verdict  # noqa: E402
import make_store  # noqa: E402
import stage_chain  # noqa: E402
import wave_context  # noqa: E402
import wave_queue  # noqa: E402

EXE = Path(os.environ.get("ATX_RESEARCH_STORE_EXE", REPO / "build-equity" / "bin" / "atx-research-store.exe"))
REGISTRY = rsi.load_registry()


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def compact(value) -> str:
    """A stored JSON text (the C++ ingest stores nlohmann's compact dump; any JSON text of the value renders the same)."""
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


def eol_of(data: bytes) -> str:
    """catalog.cpp detect_eol."""
    crlf = data.count(b"\r\n")
    lf = data.count(b"\n") - crlf
    if not lf and not crlf:
        return "none"
    if lf and crlf:
        return "mixed"
    return "crlf" if crlf else "lf"


def is_sha(value) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)


FITS = {
    "text": lambda v: isinstance(v, str),
    "sha": is_sha,
    "int": lambda v: type(v) is int and -(1 << 63) <= v < (1 << 63),
    "real": lambda v: type(v) is float and v == v and v not in (float("inf"), float("-inf")) and str(v) != "-0.0",
    "json": lambda v: True,
}


def typed_row(doc: dict, spec: list, claimed: tuple = ()) -> tuple:
    """(columns, extra) as DocFields fills them (json_text.cpp): a present value that fits its column is claimed, null
    is claimed as NULL, anything else stays in extra in document order. ``spec``: [(key, column, kind)]."""
    row, by_key = {}, {key: (column, kind) for key, column, kind in spec}
    for _, column, _ in spec:
        row[column] = None
    extra = {}
    for key, value in doc.items():
        if key in claimed:
            continue
        if key not in by_key:
            extra[key] = value
            continue
        column, kind = by_key[key]
        if value is None:
            continue
        if FITS[kind](value):
            row[column] = compact(value) if kind == "json" else value
        else:
            extra[key] = value
    return row, extra


RUN_SPEC = [("schema", "receipt_schema", "text"), ("source_sha", "source_sha", "text"),
            ("started_utc", "started_utc", "text"), ("executable_sha256", "executable_sha256", "sha"),
            ("argv_sha256", "argv_sha256", "sha"), ("attempt", "attempt", "int"), ("build_type", "build_type", "text"),
            ("limits", "limits", "json"), ("sampled_peak_tree_rss_bytes", "sampled_peak_tree_rss_bytes", "int"),
            ("minimum_system_free_bytes", "minimum_system_free_bytes", "int"), ("outcome", "outcome", "text"),
            ("exit_code", "exit_code", "int"), ("git", "git_state", "text"),
            ("dirty_outside_pathspec", "dirty_outside", "json"), ("role_id", "role_id", "text"),
            ("admission", "admission", "json"), ("error", "error", "text"), ("wall_seconds", "wall_seconds", "real"),
            ("owned_processes", "owned_processes", "json"), ("ownership_scope", "ownership_scope", "text"),
            ("logs", "logs", "json"), ("store", "store", "json")]
STAGE_SPEC = [("schema", "schema", "text"), ("chain", "chain", "text"), ("stage", "stage", "text"),
              ("index", "stage_index", "int"), ("status", "status", "text"), ("inputs", "inputs", "json"),
              ("outputs", "outputs", "json"), ("started_utc", "started_utc", "text"), ("seconds", "seconds", "real")]
BINDING_SPEC = [("schema", "schema", "text"), ("output", "output", "text"), ("spec_sha256", "spec_sha256", "sha"),
                ("spec_rule", "spec_rule", "text"), ("argv_sha256", "argv_sha256", "sha")]


def parent(rel: str) -> str:
    return rel.rsplit("/", 1)[0]


class Catalog:
    """A temp tree and a catalog store over it (make_store.py, groups catalog_core + catalog_records)."""

    def __init__(self, tmp: Path):
        self.root = tmp / "tree"
        self.root.mkdir()
        self.db = make_store.build(tmp / "catalog.sqlite", "catalog", ["catalog_core", "catalog_records"])
        self.store = rs.Store.open(self.db)

    def close(self) -> None:
        self.store.close()

    def path(self, rel: str) -> Path:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        return p

    def rel(self, path: Path) -> str:
        return Path(path).relative_to(self.root).as_posix()

    def artifact(self, rel: str, cls: str) -> tuple:
        data = (self.root / rel).read_bytes()
        doc = None
        if rel.endswith(".json"):
            doc = json.loads(data)
        schema = doc.get("schema") if isinstance(doc, dict) and isinstance(doc.get("schema"), str) else None
        self.store.upsert("artifact", {
            "path_key": rsi.path_key(rel), "path": rel, "sha256": sha(data), "bytes": len(data), "class": cls,
            "json_schema": schema, "sha_source": "verified", "declared_by": None, "eol": eol_of(data),
            "producer_key": None})
        return data, doc

    # -- families (the rows ingest_*.cpp writes) ----------------------------------------------------------------------
    def add_run(self, rel: str) -> None:
        data, doc = self.artifact(rel, "run-receipt")
        run_dir = parent(rel)
        claimed = []
        command, bindings = doc.get("command"), doc.get("bindings")
        if isinstance(command, list) and all(isinstance(a, str) for a in command):
            claimed.append("command")
            for k, arg in enumerate(command):
                self.store.insert("run_command", {"run_dir": run_dir, "ord": k, "arg": arg})
        if isinstance(bindings, list) and all(
                isinstance(b, dict) and list(b) == ["path", "sha256"] and isinstance(b["path"], str)
                and is_sha(b["sha256"]) for b in bindings):
            claimed.append("bindings")
            for k, b in enumerate(bindings):
                self.store.insert("run_binding", {"run_dir": run_dir, "ord": k, "path": b["path"],
                                                  "sha256": b["sha256"]})
        row, extra = typed_row(doc, RUN_SPEC, tuple(claimed))
        row.update(run_dir=run_dir, key_order=compact(list(doc)), extra=compact(extra) if extra else None,
                   file_sha256=sha(data))
        self.store.upsert("run", row)

    def add_run_start(self, rel: str) -> None:
        data, doc = self.artifact(rel, "run-start")
        self.store.upsert("run_start", {"run_dir": parent(rel), "file_sha256": sha(data), "doc": compact(doc)})

    def add_stage(self, rel: str) -> None:
        data, doc = self.artifact(rel, "stage-receipt")
        receipts = parent(rel)
        row, extra = typed_row(doc, STAGE_SPEC)
        row.update(state_dir=parent(receipts), file_name=rel.rsplit("/", 1)[1],
                   extra=compact(extra) if extra else None, file_sha256=sha(data))
        self.store.upsert("stage_receipt", row)

    def add_binding(self, rel: str) -> None:
        data, doc = self.artifact(rel, "cycle-binding")
        row, extra = typed_row(doc, BINDING_SPEC)
        row.update(run_dir=parent(rel), key_order=compact(list(doc)), extra=compact(extra) if extra else None,
                   file_sha256=sha(data))
        self.store.upsert("cycle_binding", row)

    def add_verdict(self, rel: str) -> None:
        data, doc = self.artifact(rel, "cycle-verdict")
        ledger = doc.get("ledger") or {}
        self.store.upsert("cycle_verdict", {
            "path": rel, "schema": doc["schema"], "cycle": doc["cycle"], "mode": doc["mode"],
            "spec_sha256": doc.get("spec_sha256"), "ledger_path": ledger.get("path"), "ledger_head": ledger.get("head"),
            "ledger_lines": ledger.get("lines"), "doc": compact(doc), "file_sha256": sha(data)})

    def add_wave_result(self, rel: str) -> None:
        data, doc = self.artifact(rel, "wave-result")
        ledger, verdict = doc.get("ledger") or {}, doc.get("verdict") or {}
        self.store.upsert("wave_result", {
            "path": rel, "schema": doc["schema"], "wave": doc["wave"], "kind": doc["kind"],
            "manifest_path": doc["manifest"]["path"], "manifest_sha256": doc["manifest"]["sha256"],
            "accepted": verdict.get("accepted"), "ledger_head": ledger.get("head"), "n_before": ledger.get("n_before"),
            "n_after": ledger.get("n_after"), "trial_id": ledger.get("trial_id"),
            "next_parent_spec": (doc.get("next_parent") or {}).get("spec"), "doc": compact(doc),
            "file_sha256": sha(data)})

    def add_candidate(self, rel: str) -> None:
        data, doc = self.artifact(rel, "wave-candidate")
        self.store.upsert("candidate", {
            "id": doc["id"], "path": rel, "status": doc["status"], "wave": doc.get("wave"),
            "dsl_sha256": doc.get("dsl_sha256"), "file_sha256": sha(data), "doc": compact(doc)})

    def add_ledger(self, rel: str) -> None:
        data, _ = self.artifact(rel, "trial-ledger")
        lines = data.decode("utf-8").split("\n")
        assert lines[-1] == ""
        for seq, line in enumerate(lines[:-1], start=1):
            rec = json.loads(line)
            self.store.insert("trial_line", {
                "ledger_path": rel, "seq": seq, "line": line, "line_sha256": sha(line.encode("utf-8")),
                "schema": rec.get("schema"), "kind": rec.get("kind"), "cell": rec.get("cell"),
                "trial_id": rec.get("trial_id"), "count": rec.get("count"), "prev_sha256": rec.get("prev_sha256")})
        self.store.upsert("ledger_state", {"ledger_path": rel, "lines": len(lines) - 1, "file_sha256": sha(data),
                                           "head_sha256": None, "head_rule": None})


def receipt_doc() -> dict:
    """run_bounded_research.py main(): the receipt dict at launch (placeholder values), completed in its finally."""
    receipt = dict(schema="atx.bounded-research-run/v1", source_sha="f" * 40, started_utc="2023-06-01T00:00:00+00:00",
                   command=["C:\\atx\\build-equity\\bin\\x.exe", "nav", "--output", "build-equity/nav"],
                   executable_sha256="9" * 64, argv_sha256="a" * 64, attempt=1, build_type="Debug",
                   bindings=[{"path": "scripts/specs/v8/x.json", "sha256": "c" * 64}],
                   limits=dict(seconds=600.0, max_rss_mib=4096, min_free_mib=512), sampled_peak_tree_rss_bytes=0,
                   minimum_system_free_bytes=123456789, outcome="launch-failed", exit_code=None,
                   git="clean in the code pathspec", dirty_outside_pathspec=[])
    receipt["role_id"] = "role_x"
    return receipt


def complete(receipt: dict) -> dict:
    receipt = dict(receipt)
    receipt.update(sampled_peak_tree_rss_bytes=52428800, exit_code=0, outcome="completed",
                   wall_seconds=12.345678901234567, owned_processes=[{"pid": 4242, "create_time": 1685577600.125}],
                   ownership_scope="sampled descendant identities", logs={"stdout.log": "d" * 64})
    return receipt


def write_receipt(path: Path, receipt: dict, newline: str | None) -> None:
    """run_bounded_research.py:497-499: json.dump(receipt, stream, indent=2) then "\\n" on open("x") in text mode
    (``newline=None``: the host's text mode; CRLF on the research host)."""
    with path.open("x", encoding="utf-8", newline=newline) as stream:
        json.dump(receipt, stream, indent=2)
        stream.write("\n")


class IdentityCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cat = Catalog(Path(self.tmp.name))

    def tearDown(self):
        self.cat.close()
        self.tmp.cleanup()

    def check(self, rel: str, cls: str):
        return rsi.check_one(self.cat.store, rel, cls, root=self.cat.root, registry=REGISTRY)

    def assert_identical(self, rel: str, cls: str, eol: str):
        self.assertIsNone(self.check(rel, cls))
        self.assertEqual(self.cat.store.get("artifact", {"path_key": rsi.path_key(rel)})["eol"], eol)

    # -- typed ------------------------------------------------------------------------------------------------------
    def test_run_receipt_typed_py_indent2_crlf_and_lf(self):
        for run, newline, eol in (("build-equity/nav-run1", "\r\n", "crlf"), ("build-equity/nav-run2", "\n", "lf")):
            write_receipt(self.cat.path(f"{run}/receipt.json"), complete(receipt_doc()), newline)
            self.cat.add_run(f"{run}/receipt.json")
            self.assert_identical(f"{run}/receipt.json", "run-receipt", eol)

    def test_run_receipt_values_that_do_not_fit_stay_in_extra(self):
        receipt = complete(receipt_doc())
        receipt.update(attempt="1", wall_seconds=12, exit_code=-0.0, store={"index": None},
                       bindings=[{"sha256": "c" * 64, "path": "x"}], later_key={"nested": [1, 2.5, None, "caf\u00e9"]})
        rel = "build-equity/nav-run3/receipt.json"
        write_receipt(self.cat.path(rel), receipt, None)   # the host's text mode
        self.cat.add_run(rel)
        extra = json.loads(self.cat.store.get("run", {"run_dir": parent(rel)})["extra"])
        self.assertEqual(list(extra), ["attempt", "bindings", "exit_code", "wall_seconds", "later_key"])
        self.assertIsNone(self.check(rel, "run-receipt"))

    def test_stage_receipts_typed_py_indent2_sorted_lf(self):
        receipts = self.cat.root / "build-equity/waves/w1/receipts"
        ok = {"schema": stage_chain.SCHEMA, "chain": "w1", "stage": "preflight", "index": 1, "status": "ok",
              "inputs": {"manifest_sha256": "1" * 64, "z": [3, 1]}, "outputs": {"n_before": 3},
              "started_utc": "2023-06-01T00:00:00+00:00", "seconds": 0.25}
        failed = {"schema": stage_chain.SCHEMA, "chain": "w1", "stage": "record", "index": 2, "status": "failed",
                  "inputs": {}, "outputs": {}, "started_utc": "2023-06-01T00:00:03+00:00", "seconds": 1e-05,
                  "error": "placeholder \u2014 failure", "code": 4}
        stage_chain.Chain.write(receipts / "01-preflight.json", ok)
        stage_chain.Chain.write(receipts / "02-record.failed-1.json", failed)
        for name in ("01-preflight.json", "02-record.failed-1.json"):
            rel = f"build-equity/waves/w1/receipts/{name}"
            self.cat.add_stage(rel)
            self.assert_identical(rel, "stage-receipt", "lf")

    def test_cycle_binding_typed_py_indent2(self):
        res = SimpleNamespace(path=lambda rel: self.cat.path(rel))
        cycle = SimpleNamespace(res=res, spec_path=None)
        st = SimpleNamespace(output="build-equity/nav-ref", phase="ref", run_dir="build-equity/nav-ref-run1",
                             argv=["py", "run_bounded_research.py", "--", "x.exe", "nav", "--output", "o"])
        cycle_resume.write_binding(cycle, st)   # Path.write_text in the host's text mode
        rel = "build-equity/nav-ref-run1/cycle_binding.json"
        self.cat.add_binding(rel)
        self.assertIsNone(self.check(rel, "cycle-binding"))
        # A nav binding (spec digest and rule), cycle_resume.py:185-189's expression in Windows text mode.
        doc = {"schema": cycle_resume.SCHEMA, "output": "build-equity/nav", "spec_sha256": "c" * 64,
               "spec_rule": cycle_resume.SPEC_RULE, "argv_sha256": "a" * 64}
        rel = "build-equity/nav-run1/cycle_binding.json"
        self.cat.path(rel).write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8", newline="\r\n")
        self.cat.add_binding(rel)
        self.assert_identical(rel, "cycle-binding", "crlf")

    # -- doc --------------------------------------------------------------------------------------------------------
    def test_run_start_doc_py_indent2(self):
        rel = "build-equity/nav-run1/start.json"
        # run_bounded_research.py:434 (host text mode), then the same text in Windows text mode.
        self.cat.path(rel).write_text(json.dumps(receipt_doc(), indent=2) + "\n", encoding="utf-8")
        self.cat.add_run_start(rel)
        self.assertIsNone(self.check(rel, "run-start"))
        rel = "build-equity/nav-run2/start.json"
        self.cat.path(rel).write_text(json.dumps(receipt_doc(), indent=2) + "\n", encoding="utf-8", newline="\r\n")
        self.cat.add_run_start(rel)
        self.assert_identical(rel, "run-start", "crlf")

    def test_verdict_and_copy_doc_py_indent2_lf(self):
        doc = {"schema": cycle_verdict.VERDICT_SCHEMA, "cycle": "c1", "mode": "run", "spec_sha256": "c" * 64,
               "admission": [{"id": "a", "admitted": True}], "marginal": [],
               "phases": [{"name": "nav", "seconds": 1.5, "peak_mib": 50}],
               "paired": {"dsr": 0.5, "se": 0.25, "cbb_ci": [0.125, 0.875], "lw_p": 1e-300},
               "ledger": {"path": "build-equity/trials.jsonl", "head": "e" * 64, "lines": 5}}
        text = json.dumps(doc, indent=2) + "\n"
        rel = "build-equity/cycle-c1/cycle_verdict.json"
        # cycle_verdict.write_verdict (cycle_verdict.py:146-148), then its keep_copy.
        self.cat.path(rel).write_text(text, encoding="utf-8", newline="\n")
        cycle = SimpleNamespace(res=SimpleNamespace(path=lambda r: self.cat.root / r),
                                cycle_dir=lambda: "build-equity/cycle-c1")
        copy = cycle_verdict.keep_copy(cycle, doc["mode"], text)
        self.assertEqual(copy, "build-equity/cycle-c1/verdicts/run-1.json")
        for path in (rel, copy):
            self.cat.add_verdict(path)
            self.assert_identical(path, "cycle-verdict", "lf")

    def test_wave_result_doc_py_indent2_lf(self):
        doc = {"schema": "atx.wave-result/v1", "wave": "w1", "kind": "rule",
               "manifest": {"path": "scripts/specs/v8/waves/w1.json", "sha256": "1" * 64, "commit": None},
               "stats": {"cell": {"placeholder": 1.0, "big": 1e16, "small": 5e-324, "int": 2 ** 63}},
               "verdict": {"accepted": False, "reason": "placeholder"},
               "ledger": {"path": "build-equity/trials.jsonl", "head": "e" * 64, "n_before": 3, "n_after": 4,
                          "trial_id": "t5"},
               "next_parent": {"spec": "scripts/specs/v8/w1.json", "library": "lib"},
               "timings": [{"phase": "nav", "seconds": 12.345678901234567, "peak_mib": 50}]}
        wave = SimpleNamespace(path=lambda rel: self.cat.root / rel)
        written = wave_context.Wave.write_json(wave, "build-equity/waves/w1/wave-result.json", doc)
        rel = "build-equity/waves/w1/wave-result.json"
        self.assertEqual(written, sha((self.cat.root / rel).read_bytes()))
        self.cat.add_wave_result(rel)
        self.assert_identical(rel, "wave-result", "lf")

    def test_candidates_doc_py_indent2_noascii_lf(self):
        for cid, status in (("cand_a", "pinned"), ("cand_b", "proposed")):
            c = {"id": cid, "dsl": "rank(close)", "dsl_sha256": sha(b"rank(close)"), "theme": "fixture",
                 "citation": "Fixture (2020) \u2014 placeholder \U0001F600", "schema": "atx.wave-candidate/v1",
                 "status": status, "wave": None, "history": [{"status": "proposed", "at": "2023-05-01", "by": "x"}]}
            rel = self.cat.rel(wave_queue.write(self.cat.root, c))
            self.cat.add_candidate(rel)
            self.assert_identical(rel, "wave-candidate", "lf")

    # -- lines ------------------------------------------------------------------------------------------------------
    def test_trial_ledger_py_compact_sorted_lines_lf(self):
        path = self.cat.path("build-equity/trials.jsonl")
        rows = [{"schema": backtest_integrity.LEDGER_SCHEMA, "trial_id": f"t{k}", "cell": f"cell-{k}",
                 "kind": "fixture", "count": k, "z": {"b": 1.5, "a": None}} for k in range(1, 6)]
        backtest_integrity.ledger_append(path, rows[:2], chain=False)
        backtest_integrity.ledger_append(path, rows[2:], chain=True)
        self.cat.add_ledger("build-equity/trials.jsonl")
        self.assert_identical("build-equity/trials.jsonl", "trial-ledger", "lf")

    # -- what the checker reports -------------------------------------------------------------------------------------
    def one_receipt_and_verdict(self) -> tuple:
        receipt = "build-equity/nav-run1/receipt.json"
        write_receipt(self.cat.path(receipt), complete(receipt_doc()), "\r\n")
        self.cat.add_run(receipt)
        verdict = "build-equity/cycle-c1/cycle_verdict.json"
        doc = {"schema": cycle_verdict.VERDICT_SCHEMA, "cycle": "c1", "mode": "run", "paired": {"dsr": 0.5}}
        self.cat.path(verdict).write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8", newline="\n")
        self.cat.add_verdict(verdict)
        return receipt, verdict

    def test_one_byte_edit_on_disk_is_reported(self):
        for rel, cls in zip(self.one_receipt_and_verdict(), ("run-receipt", "cycle-verdict")):
            self.assertIsNone(self.check(rel, cls))
            file = self.cat.root / rel
            data = bytearray(file.read_bytes())
            at = data.index(b'"schema"') + 2
            data[at] ^= 0x20   # "schema" -> "sChema"
            file.write_bytes(bytes(data))
            found = self.check(rel, cls)
            self.assertIsNotNone(found)
            self.assertIn(f"differs from the render at byte {at} ", found.reason)
            # Without the root, the rows still render the catalogued bytes.
            self.assertIsNone(rsi.check_one(self.cat.store, rel, cls, registry=REGISTRY))

    def test_one_byte_edit_of_a_row_is_reported(self):
        receipt, verdict = self.one_receipt_and_verdict()
        con = self.cat.store.connection
        con.execute("UPDATE run SET outcome = 'complete!' WHERE run_dir = ?", (parent(receipt),))
        con.execute("UPDATE cycle_verdict SET doc = replace(doc, '0.5', '0.6') WHERE path = ?", (verdict,))
        con.execute("UPDATE run_command SET arg = 'mav' WHERE run_dir = ? AND ord = 1", (parent(receipt),))
        for rel, cls in ((receipt, "run-receipt"), (verdict, "cycle-verdict")):
            found = rsi.check_one(self.cat.store, rel, cls, registry=REGISTRY)
            self.assertIsNotNone(found)
            self.assertIn("rendered sha256", found.reason)
            self.assertNotIn("0.6", found.reason)   # digests and offsets only, never a value

    def test_eol_stale_rows_and_unrenderable_keys_are_reported(self):
        receipt, verdict = self.one_receipt_and_verdict()
        con = self.cat.store.connection
        key = rsi.path_key(receipt)
        cases = [
            ("UPDATE artifact SET eol = 'lf' WHERE path_key = ?", (key,), "rendered sha256"),
            ("UPDATE artifact SET eol = 'mixed' WHERE path_key = ?", (key,), "line end 'mixed' cannot be rendered"),
            ("UPDATE artifact SET sha_source = 'declared' WHERE path_key = ?", (key,), "sha_source declared"),
            ("UPDATE run SET file_sha256 = ? WHERE run_dir = ?", ("0" * 64, parent(receipt)), "stale row"),
            ("UPDATE run SET key_order = json_insert(key_order, '$[#]', 'unknown_key') WHERE run_dir = ?",
             (parent(receipt),), "key 'unknown_key' has no column"),
            ("DELETE FROM run WHERE run_dir = ?", (parent(receipt),), "0 run rows"),
        ]
        for sql, args, expected in cases:
            with self.subTest(expected=expected):
                con.execute("SAVEPOINT s")
                con.execute(sql, args)
                found = self.check(receipt, "run-receipt")
                con.execute("ROLLBACK TO s")
                con.execute("RELEASE s")
                self.assertIsNotNone(found)
                self.assertIn(expected, found.reason)
        self.assertIsNone(self.check(receipt, "run-receipt"))
        self.assertIn("catalogued as class cycle-verdict", self.check(verdict, "run-receipt").reason)
        self.assertIn("no artifact row", self.check("build-equity/absent.json", "run-receipt").reason)

    def test_sealed_paths_never_opened(self):
        rel = "build-equity/role-2023-2024/cycle_verdict.json"
        doc = {"schema": cycle_verdict.VERDICT_SCHEMA, "cycle": "c", "mode": "run"}
        self.cat.path(rel).write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8", newline="\n")
        self.cat.add_verdict(rel)
        self.assertIsNone(rsi.check_one(self.cat.store, rel, "cycle-verdict", registry=REGISTRY))
        self.assertIn("sealed path", self.check(rel, "cycle-verdict").reason)

    def test_cli_counts_and_exit_codes(self):
        receipt, _ = self.one_receipt_and_verdict()
        args = ["--catalog", str(self.cat.db), "--root", str(self.cat.root)]
        out = io.StringIO()
        self.assertEqual(rsi.main(args, out=out), rsi.EXIT_OK)
        self.assertIn("class run-receipt checked 1 mismatches 0\n", out.getvalue())
        self.assertIn("class cycle-verdict checked 1 mismatches 0\n", out.getvalue())
        self.assertTrue(out.getvalue().endswith("total checked 2 mismatches 0\n"))
        out = io.StringIO()
        self.assertEqual(rsi.main([*args, "--classes", "run-receipt"], out=out), rsi.EXIT_OK)
        self.assertTrue(out.getvalue().endswith("total checked 1 mismatches 0\n"))
        (self.cat.root / receipt).write_bytes((self.cat.root / receipt).read_bytes().replace(b"\r\n", b"\n"))
        out = io.StringIO()
        self.assertEqual(rsi.main(args, out=out), rsi.EXIT_MISMATCH)
        self.assertIn(f"mismatch {receipt} [run-receipt]: file on disk differs", out.getvalue())
        self.assertTrue(out.getvalue().endswith("total checked 2 mismatches 1\n"))
        with self.assertRaises(SystemExit):
            rsi.main(["--root", "x"])
        self.assertEqual(rsi.main([*args, "--classes", "run-log"], out=io.StringIO()), rsi.EXIT_USAGE)
        self.assertEqual(rsi.main(["--catalog", str(self.cat.root / "absent.sqlite"), "--root", "x"],
                                  out=io.StringIO()), rsi.EXIT_USAGE)

    def test_registry_refuses_unknown_rules(self):
        doc = json.loads(rsi.DEFAULT_REGISTRY.read_text(encoding="utf-8"))
        doc["classes"][0]["render"] = {"mode": "typed", "rule": "py-indent4"}
        bad = Path(self.tmp.name) / "classes.json"
        bad.write_text(json.dumps(doc), encoding="utf-8")
        with self.assertRaises(ValueError):
            rsi.load_registry(bad)
        self.assertEqual(rsi.rendered_classes(REGISTRY),
                         ["run-receipt", "run-start", "stage-receipt", "cycle-binding", "cycle-verdict", "wave-result",
                          "wave-candidate", "trial-ledger"])


@unittest.skipUnless(EXE.is_file(), f"{EXE} not built (target atx-research-store)")
class FixtureChain(unittest.TestCase):
    """The C++ catalog over the committed fixture tree, then this checker: 0 mismatches (root's fixture chain)."""

    def test_cpp_catalog_renders_identically(self):
        tree = FIXTURES / "tree"
        with tempfile.TemporaryDirectory() as tmp:
            db = Path(tmp) / "catalog.sqlite"
            for argv in (["init", "--catalog", str(db)],
                         ["catalog", "--catalog", str(db), "--root", str(tree), "--classes",
                          str(rsi.DEFAULT_REGISTRY)]):
                done = subprocess.run([str(EXE), *argv], capture_output=True, text=True, timeout=120)
                self.assertEqual(done.returncode, 0, done.stderr)
            out = io.StringIO()
            code = rsi.main(["--catalog", str(db), "--root", str(tree)], out=out)
            self.assertEqual(code, rsi.EXIT_OK, out.getvalue())
            self.assertTrue(out.getvalue().endswith("total checked 12 mismatches 0\n"), out.getvalue())


if __name__ == "__main__":
    unittest.main()
