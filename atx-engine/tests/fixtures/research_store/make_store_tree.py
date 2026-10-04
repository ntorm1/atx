"""Test-only: write the synthetic research tree ``tree/`` and the legacy record store ``legacy_records/`` (P9 SQL2).

The catalog gtests (atx-engine/tests/research/research_catalog_*_test.cpp), root's fixture chain run
(``atx-research-store catalog --root .../tree`` then ``research_store_identity.py``) and the identity pytests read
these bytes. Every artifact is written by its real writer where the writer is importable as a function
(``stage_chain.Chain.write``, ``wave_queue.write``, ``cycle_verdict.keep_copy``, ``backtest_integrity.ledger_append``,
``record_store.RecordStore.put``); where the writer is a script's ``main`` (``run_bounded_research.py``) or needs a
whole cycle (``cycle_resume.write_binding``, ``cycle_verdict.write_verdict``, ``wave_context.Wave.write_json``) the
exact write expression of that writer is used, cited at the call. Windows text mode (CRLF) is written explicitly
(``newline="\\r\\n"``) where the writer relies on it, so the committed bytes do not depend on the host. Nothing here is
real data: every number is a placeholder. ``tree/.gitattributes`` (``* -text``) keeps the line ends as written.

Contents (what the gtests rely on):
  scripts/specs/v8/fx-base.json             cycle spec: inputs library / role (ok), reference_daily (missing),
                                            fields.manifest_sha256 (ok), nav / fields outputs, summ.ledger
  scripts/specs/v8/fx-tmpl.json             template: locked reference_cell (ok), change.inputs.extra (outside root)
  scripts/specs/v8/waves/fx-w1.json         wave manifest: fields (ok), rule_cell template (ok), out_dir, ledger
  scripts/specs/v8/candidates/*.json        two candidates (wave_queue.write)
  atx-impl/strategies/libraries/lib-fx.json IC library (pinned)
  atx-engine/tools/field_registry.json      field registry (seeded; pinned by the fields manifest)
  build-equity/fx-role/role.json            role manifest (pinned)
  build-equity/fx-fields/manifest.json      fields manifest declaring f1.f64 / f2.f64 (absent: the gtests create them,
                                            PAYLOAD_SIZES bytes of zeros, over 16 MiB); field sources outside the
                                            root, in it but named by nothing else (fx-sources/), and role.json
  build-equity/fx-nav/summary.json          NAV summary (pinned by the template); extra_nan.json (a NaN token)
  build-equity/fx-nav-run1/                 start.json, receipt.json, cycle_binding.json (CRLF), stdout/stderr.log
  build-equity/cycle-fx-base/               cycle_verdict.json + verdicts/run-1.json (placeholder numbers)
  build-equity/waves/fx-w1/                 receipts/01-preflight, 02-run, 03-record.failed-1 (LF, sorted keys),
                                            wave-result.json, wave-log.md, role-2023-2024/bad.json (seal decoy:
                                            invalid JSON that would fail an ingest)
  build-equity/trials.jsonl                 5 lines: 2 legacy (unchained) + 3 chained (ledger_append)
  build-equity/mega-fx-1-receipt.json       research-build receipt (PowerShell layout, CRLF)
  build-equity/unpinned/notes.json          named by nothing: listed (outside-roots), never opened
legacy_records/                             two record-store partitions ("" and part-a) for `cache init --import`

Usage: python make_store_tree.py   (rewrites tree/ and legacy_records/ beside this file)
"""
from __future__ import annotations

import hashlib
import json
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
TREE = HERE / "tree"
LEGACY = HERE / "legacy_records"
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO / "atx-engine" / "tools"))
sys.path.insert(0, str(REPO / "atx-impl" / "tools"))

import backtest_integrity  # noqa: E402
import cycle_verdict  # noqa: E402
import record_store  # noqa: E402
import stage_chain  # noqa: E402
import wave_queue  # noqa: E402

MIB = 1024 * 1024
PAYLOAD_SIZES = {"f1.f64": 16 * MIB + 8, "f2.f64": 16 * MIB + 16}   # zero bytes; the gtests create them
OUTSIDE = "C:\\elsewhere\\not-in-the-tree"


def sha_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha_file(path: Path) -> str:
    return sha_bytes(path.read_bytes())


def zeros_sha(size: int) -> str:
    h = hashlib.sha256()
    chunk = bytes(MIB)
    left = size
    while left:
        n = min(left, len(chunk))
        h.update(chunk[:n])
        left -= n
    return h.hexdigest()


def write_lf(rel: str, text: str) -> Path:
    path = TREE / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")
    return path


def write_crlf(rel: str, text: str) -> Path:
    """Windows text mode, as Path.write_text / open("x") without newline= writes on the research host."""
    path = TREE / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\r\n")
    return path


class _Res:
    def path(self, rel: str) -> Path:
        return TREE / rel


class _Cycle:
    res = _Res()

    @staticmethod
    def cycle_dir() -> str:
        return "build-equity/cycle-fx-base"


def inputs() -> dict:
    lib = write_lf("atx-impl/strategies/libraries/lib-fx.json", json.dumps(
        {"schema": "atx.dsl-ic-library/v1", "name": "lib-fx", "candidates": [{"id": "fx_alpha_a", "dsl": "rank(x)"}]},
        indent=2) + "\n")
    role = write_lf("build-equity/fx-role/role.json", json.dumps(
        {"schema": "atx.recent-research-role/v1", "role": "fx",
         "files": {"member.u8": {"bytes": 8, "sha256": "0" * 64}}},
        indent=2) + "\n")
    registry = write_lf("atx-engine/tools/field_registry.json", json.dumps(
        {"schema": "atx.field-registry/v1", "fields": [{"name": "f1"}, {"name": "f2"}]}, indent=2) + "\n")
    summary = write_lf("build-equity/fx-nav/summary.json", json.dumps(
        {"schema": "atx.dsl-nav-replay-summary/v1", "placeholder": True}, indent=2) + "\n")
    # A NaN token (json.dumps' default allow_nan): the catalog keeps it as an artifact only (unparsed).
    write_lf("build-equity/fx-nav/extra_nan.json",
             json.dumps({"schema": "atx.nav-v7-extras/v1", "x": float("nan")}) + "\n")
    return {"lib": lib, "role": role, "registry": registry, "summary": summary}


def fields_manifest(registry: Path, role: Path) -> Path:
    # A builder input inside the root that nothing else names: its field-source pin is recorded, never followed.
    source = write_lf("build-equity/fx-sources/src.json", json.dumps({"source": "placeholder"}) + "\n")
    files = {name: {"bytes": size, "sha256": zeros_sha(size)} for name, size in PAYLOAD_SIZES.items()}
    producer = {"kind": "engine", "exe_sha256": "e" * 64, "git_sha": "fixture", "build_type": "Debug",
                "receipt_sha256": "d" * 64}
    m = {"schema": "atx.research-role-fields/v1", "status": "complete",
         "role": {"path": "build-equity/fx-role", "manifest_sha256": "1" * 64},
         "seal": {"exclusive_end": "2024-01-01", "rule": "placeholder"},
         "fields": [
             {"name": "f1", "file": "f1.f64", "dtype": "<f8", "sha256": files["f1.f64"]["sha256"], "producer": producer,
              "formula_sha256": "2" * 64,
              "sources": [{"path": OUTSIDE + "\\src.parquet", "bytes": 1, "sha256": "3" * 64},
                          {"path": "build-equity/fx-sources/src.json", "bytes": 1, "sha256": sha_file(source)}]},
             {"name": "f2", "file": "f2.f64", "dtype": "<f8", "sha256": files["f2.f64"]["sha256"],
              "producer": {"module": "prepare_research_fields", "code_sha256": "4" * 64},
              "reused_from": {"path": "build-equity/fx-fields-old", "manifest_sha256": "5" * 64},
              "sources": [{"path": "build-equity/fx-role/role.json", "bytes": 1, "sha256": sha_file(role)}]}],
         "files": files,
         "registry": {"path": "atx-engine/tools/field_registry.json", "sha256": sha_file(registry)},
         "engine": {"name": "atx-research-fields", "exe_sha256": "e" * 64, "git_sha": "fixture", "build_type": "Debug"},
         "receipt_sha256": "d" * 64}
    # A2's manifest_text: sorted keys, two-space indent, ASCII escapes, final newline.
    return write_lf("build-equity/fx-fields/manifest.json", json.dumps(m, indent=2, sort_keys=True) + "\n")


def specs(t: dict, manifest: Path) -> dict:
    spec = {"schema": "atx.research-cycle-spec/v1", "name": "fx-base", "description": "synthetic fixture spec",
            "inputs": {"library": {"path": "atx-impl/strategies/libraries/lib-fx.json", "sha256": sha_file(t["lib"])},
                       "role": {"dir": "build-equity/fx-role", "path": "build-equity/fx-role/role.json",
                                "sha256": sha_file(t["role"])},
                       "reference_daily": {"path": "build-equity/fx-nav/daily_missing.csv", "sha256": "7" * 64},
                       "unlocked": {"path": "build-equity/fx-nav/summary.json", "sha256": None}},
            "fields": {"output": "build-equity/fx-fields", "manifest_sha256": sha_file(manifest), "list": ["f1", "f2"]},
            "nav": {"output": "build-equity/fx-nav", "rule": "fixture"},
            "summ": {"ledger": "build-equity/trials.jsonl"}}
    base = write_lf("scripts/specs/v8/fx-base.json", json.dumps(spec, indent=2) + "\n")
    tmpl = {"schema": "atx.research-cycle-template/v1", "name": "fx-tmpl", "description": "synthetic template",
            "parent": "scripts/specs/v8/fx-base.json",
            "change": {"set": {"nav.output": "build-equity/fx-nav-b"},
                       "inputs": {"extra": {"path": OUTSIDE + "\\extra.json", "sha256": "8" * 64}}},
            "locked": {"reference_cell": {"path": "build-equity/fx-nav/summary.json",
                                          "sha256": sha_file(t["summary"])}}}
    template = write_lf("scripts/specs/v8/fx-tmpl.json", json.dumps(tmpl, indent=2) + "\n")
    wave = {"schema": "atx.research-wave/v1", "wave": "fx-w1", "description": "synthetic wave",
            "parent": {"spec": "scripts/specs/v8/fx-base.json", "library": "lib-fx"},
            "fields": {"dir": "build-equity/fx-fields", "manifest_sha256": sha_file(manifest)},
            "rule_cell": {"template": "scripts/specs/v8/fx-tmpl.json", "template_sha256": sha_file(template)},
            "ledger": "build-equity/trials.jsonl", "out_dir": "build-equity/waves/fx-w1"}
    manifest_path = write_lf("scripts/specs/v8/waves/fx-w1.json", json.dumps(wave, indent=2) + "\n")
    for cid, status in (("fx_alpha_a", "pinned"), ("fx_alpha_b", "proposed")):
        c = {"id": cid, "dsl": "rank(close)", "dsl_sha256": sha_bytes(b"rank(close)"), "theme": "fixture",
             "tier": "B", "prior_sign": 1, "citation": "Fixture (2020) \u2014 placeholder", "origin": "prior",
             "hypothesis": "h-fx", "schema": "atx.wave-candidate/v1", "status": status, "wave": None,
             "history": [{"status": "proposed", "at": "2023-05-01", "by": "lane"}]}
        if status == "pinned":
            c["history"].append({"status": "pinned", "at": "2023-05-02", "by": "pm", "note": "ruling fx"})
        wave_queue.write(TREE, c)
    return {"base": base, "template": template, "wave": manifest_path}


def run_dir(base: Path) -> None:
    rel = "build-equity/fx-nav-run1"
    command = ["C:\\atx\\build-equity\\bin\\atx-equity-strategy-targets.exe", "nav", "--output", "build-equity/fx-nav"]
    # run_bounded_research.py main(): the receipt dict at launch, start.json by Path.write_text (text mode).
    receipt = dict(schema="atx.bounded-research-run/v1", source_sha="f" * 40, started_utc="2023-06-01T00:00:00+00:00",
                   command=command, executable_sha256="9" * 64, argv_sha256="a" * 64, attempt=1, build_type="Debug",
                   bindings=[{"path": "scripts/specs/v8/fx-base.json", "sha256": sha_file(base)},
                             {"path": OUTSIDE + "\\bound.json", "sha256": "b" * 64}],
                   limits=dict(seconds=600.0, max_rss_mib=4096, min_free_mib=512), sampled_peak_tree_rss_bytes=0,
                   minimum_system_free_bytes=123456789, outcome="launch-failed", exit_code=None,
                   git="clean in the code pathspec", dirty_outside_pathspec=["build-equity/x"])
    receipt["role_id"] = "fx_role"
    write_crlf(f"{rel}/start.json", json.dumps(receipt, indent=2) + "\n")
    write_lf(f"{rel}/stdout.log", "nav: placeholder\n")
    write_lf(f"{rel}/stderr.log", "")
    receipt["sampled_peak_tree_rss_bytes"] = 52428800
    receipt["minimum_system_free_bytes"] = 1073741824
    receipt["exit_code"] = 0
    receipt["outcome"] = "completed"
    receipt["wall_seconds"] = 12.345678901234567
    receipt["owned_processes"] = [{"pid": 4242, "create_time": 1685577600.125}]
    receipt["ownership_scope"] = "sampled descendant identities; commands must not detach unsampled children"
    receipt["logs"] = {name: sha_file(TREE / rel / name) for name in ("stdout.log", "stderr.log")}
    # finally: json.dump(receipt, stream, indent=2); stream.write("\n") on open("x") in text mode.
    write_crlf(f"{rel}/receipt.json", json.dumps(receipt, indent=2) + "\n")
    # cycle_resume.write_binding: json.dumps(doc, indent=2) + "\n" by Path.write_text (text mode).
    doc = {"schema": "atx.cycle-nav-binding/v1", "output": "build-equity/fx-nav", "spec_sha256": "c" * 64,
           "spec_rule": "spec-digest-v1", "argv_sha256": "a" * 64}
    write_crlf(f"{rel}/cycle_binding.json", json.dumps(doc, indent=2) + "\n")


def ledger() -> Path:
    path = TREE / "build-equity" / "trials.jsonl"
    path.parent.mkdir(parents=True, exist_ok=True)
    rows = [{"schema": backtest_integrity.LEDGER_SCHEMA, "trial_id": f"fx-t{k}", "cell": f"fx-cell-{k}",
             "kind": "fixture", "count": k} for k in range(1, 6)]
    backtest_integrity.ledger_append(path, rows[:2], chain=False)   # two legacy (unchained) lines
    backtest_integrity.ledger_append(path, rows[2:], chain=True)    # three chained lines
    lock = path.with_name(path.name + ".lock")
    if lock.exists():
        lock.unlink()
    return path


def verdict(ledger_path: Path) -> None:
    head = backtest_integrity.ledger_head(ledger_path)
    doc = {"schema": cycle_verdict.VERDICT_SCHEMA, "cycle": "fx-base", "mode": "run", "spec_sha256": "c" * 64,
           "admission": [{"id": "fx_alpha_a", "admitted": True}], "marginal": [],
           "phases": [{"name": "nav", "seconds": 1.5, "peak_mib": 50}],
           "paired": {"dsr": 0.5, "se": 0.25, "cbb_ci": [0.125, 0.875], "lw_p": 0.5},
           "ledger": {"path": "build-equity/trials.jsonl", "head": head, "lines": 5}}
    text = json.dumps(doc, indent=2) + "\n"
    # cycle_verdict.write_verdict: path.write_text(text, encoding="utf-8", newline="\n"), then keep_copy.
    write_lf("build-equity/cycle-fx-base/cycle_verdict.json", text)
    cycle_verdict.keep_copy(_Cycle(), doc["mode"], text)


def wave(manifest: Path, ledger_path: Path) -> None:
    out = "build-equity/waves/fx-w1"
    bodies = [
        ("01-preflight.json", {"schema": stage_chain.SCHEMA, "chain": "fx-w1", "stage": "preflight", "index": 1,
                               "status": "ok", "inputs": {"manifest_sha256": sha_file(manifest)},
                               "outputs": {"n_before": 3}, "started_utc": "2023-06-01T00:00:00+00:00",
                               "seconds": 0.25}),
        ("02-run.json", {"schema": stage_chain.SCHEMA, "chain": "fx-w1", "stage": "run", "index": 2, "status": "ok",
                         "inputs": {"previous_receipt_sha256": None}, "outputs": {"runs": ["fx-nav-run1"]},
                         "started_utc": "2023-06-01T00:00:01+00:00", "seconds": 1.5}),
        ("03-record.failed-1.json", {"schema": stage_chain.SCHEMA, "chain": "fx-w1", "stage": "record", "index": 3,
                                     "status": "failed", "inputs": {}, "outputs": {},
                                     "started_utc": "2023-06-01T00:00:03+00:00", "seconds": 0.0,
                                     "error": "placeholder failure", "code": 4})]
    receipts = TREE / out / "receipts"
    for name, body in bodies:
        if name == "02-run.json":
            body["inputs"]["previous_receipt_sha256"] = sha_file(receipts / "01-preflight.json")
        stage_chain.Chain.write(receipts / name, body)   # sorted keys, indent 2, LF; exclusive create
    doc = {"schema": "atx.wave-result/v1", "wave": "fx-w1", "kind": "rule", "description": "synthetic wave",
           "manifest": {"path": "scripts/specs/v8/waves/fx-w1.json", "sha256": sha_file(manifest), "commit": None},
           "library": None, "template": "scripts/specs/v8/fx-tmpl.json",
           "parent": {"spec": "scripts/specs/v8/fx-base.json", "library": "lib-fx"}, "screen": None,
           "stats": {"cell": {"placeholder": 1.0}}, "paired": {"dsr": 0.5}, "dsr": None, "pbo": None,
           "verdict": {"accepted": False, "reason": "placeholder"},
           "ledger": {"path": "build-equity/trials.jsonl", "lines_before": 3, "lines_after": 5,
                      "head": backtest_integrity.ledger_head(ledger_path), "n_before": 3, "n_after": 4,
                      "trial_id": "fx-t5", "admission_lines": []},
           "next_parent": {"spec": "scripts/specs/v8/fx-base.json", "library": "lib-fx"},
           "timings": [{"phase": "nav", "run_dir": "build-equity/fx-nav-run1", "outcome": "completed", "exit_code": 0,
                        "seconds": 12.345678901234567, "peak_mib": 50, "executable_sha256": "9" * 64}],
           "receipts": {p.stem: sha_file(p) for p in sorted(receipts.glob("*.json")) if ".failed-" not in p.name}}
    # wave_context.Wave.write_json: json.dumps(doc, indent=2, allow_nan=False) + "\n", newline="\n".
    write_lf(f"{out}/wave-result.json", json.dumps(doc, indent=2, allow_nan=False) + "\n")
    write_lf(f"{out}/wave-log.md", "## fx-w1 (placeholder)\n")
    write_lf(f"{out}/role-2023-2024/bad.json", "{this is not json, and the seal guard never opens it\n")


def build_receipt() -> None:
    text = "\n".join([
        "{",
        '    "Source":  "' + "f" * 40 + '",',
        '    "Preset":  "equity-dev",',
        '    "ExitCode":  0,',
        '    "WallSeconds":  102.2326265,',
        '    "Tag":  "fx-1",',
        '    "Executables":  {',
        '                        "atx-research-store":  {',
        '                                                   "Path":  '
        '"C:\\\\atx\\\\build-equity\\\\bin\\\\atx-research-store.exe",',
        '                                                   "Sha256":  "' + "A" * 64 + '"',
        "                                               }",
        "                    }",
        "}"]) + "\n"
    write_crlf("build-equity/mega-fx-1-receipt.json", text)   # ConvertTo-Json | Set-Content -Encoding ASCII


def legacy_records() -> None:
    """Two record-store partitions written by record_store.RecordStore.put (no index: the JSON file store)."""
    if LEGACY.exists():
        shutil.rmtree(LEGACY)
    body = {"zeta": 0.1, "alpha": [1e15, 1e16, 1e-05, 123.456, -0.0, 5e-324, 1.7976931348623157e308],
            "text": "caf\u00e9 \U0001F600 ctl\u0001 del\u007f quote\" back\\slash", "big": 2 ** 63, "neg": -(2 ** 63),
            "nested": {"b": True, "a": None}}
    assert record_store.RecordStore(LEGACY).put("factor", {"window": "w", "id": "x1", "n": 3}, body)
    assert record_store.RecordStore(LEGACY / "part-a").put("aim", {"z": 1, "a": [2, 1]}, {"weights": [0.25, 0.75]})
    (LEGACY / ".gitattributes").write_text("* -text\n", encoding="utf-8", newline="\n")


def main() -> int:
    if TREE.exists():
        shutil.rmtree(TREE)
    TREE.mkdir(parents=True)
    (TREE / ".gitattributes").write_text("* -text\n", encoding="utf-8", newline="\n")
    t = inputs()
    manifest = fields_manifest(t["registry"], t["role"])
    s = specs(t, manifest)
    run_dir(s["base"])
    ledger_path = ledger()
    verdict(ledger_path)
    wave(s["wave"], ledger_path)
    build_receipt()
    write_lf("build-equity/unpinned/notes.json", json.dumps({"note": "named by nothing"}) + "\n")
    legacy_records()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
