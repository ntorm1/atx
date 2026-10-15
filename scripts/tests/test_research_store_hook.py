"""P9 SQL3: the record-store writer hook and the run receipt's store block (scripts/research_store_hook.py; rulings
SQL-2, SQL-6, SQL-11, W3-4).

  test_selection_finds_the_indexes_a_store_could_select   d/, d/.. (RecordStore's two places), d/*/; --flag=value;
                                                          non-directories ignored; None when nothing; catalog on
  test_runner_without_catalog_or_index_writes_todays_bytes start.json / receipt.json = the pre-change golden
  test_runner_records_an_index_and_the_catalog            the store block, last in both files; a failing exe warns
                                                          once per file and changes no exit code
  test_hook_off_spawns_and_imports_nothing                no catalog: no subprocess, no sqlite3, no identity module
  test_one_ingest_per_write_in_a_fake_wave                fake exe: one ingest per receipt / candidate / wave result
                                                          written, with class, root-relative path, --root, --catalog,
                                                          --classes; the wave result whole when ingested (item 5)
  test_fake_wave_bytes_equal_with_a_failing_store         no catalog vs a failing exe at the same root: every file
                                                          equal (time keys aside), exit codes equal, one warning
                                                          per write
  test_write_binding_ingests_its_binding                  item 5, cycle_resume.write_binding: one ingest (class
                                                          cycle-binding) of the whole file, absolute or relative root
  test_write_verdict_ingests_the_verdict_then_its_copy    item 5, cycle_verdict.write_verdict: the verdict, then (keep)
                                                          its copy, each one ingest of the whole file
  test_keep_copy_ingests_each_copy                        item 5, cycle_verdict.keep_copy: one ingest per copy written
  test_failures_are_one_warning_each                      ingest exit 3, a timeout, a check mismatch: one line each
  test_real_store_exe_ingests_every_class                 the built atx-research-store in a temp root: every class
                                                          ingested and re-rendered (item 5's writers by their own
                                                          calls); a non-canonical file and a re-queued candidate id
                                                          (N3) are one warning each

Every file is synthetic (the tiny-world fake wave of wave_fixture, sessions 2020-2021); no data of the repository is
read. The last case copies the built exe (ATX_RESEARCH_STORE_EXE, default build-equity/bin) into a temp root and is
skipped when it is not built.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_research_store_hook.py
"""
from __future__ import annotations

import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
from types import SimpleNamespace

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
from cycle.resolver import Resolver  # noqa: E402
import cycle_resume  # noqa: E402
import cycle_verdict  # noqa: E402
import research_store_hook as H  # noqa: E402
import research_wave  # noqa: E402
import test_wave_driver as TW  # noqa: E402  (queued_root, manifest, ADMIT_ALL)
import wave_fixture as F  # noqa: E402
import wave_queue as WQ  # noqa: E402
from wave_context import stage_chain as SC  # noqa: E402

REPO = HERE.parents[1]
RUNNER = HERE.parent / "run_bounded_research.py"
EXE = Path(os.environ.get("ATX_RESEARCH_STORE_EXE", REPO / "build-equity" / "bin" / "atx-research-store.exe"))
WARN = "store: WARNING "
# The pre-change runner's start.json / receipt.json for `-- python -c pass` under --no-git, volatile values replaced
# (captured at 93760a1e before any SQL3 edit; sql3-evidence/runner-golden-prechange.txt in the lane's sprint dir).
VOLATILE = ("started_utc", "executable_sha256", "sampled_peak_tree_rss_bytes", "minimum_system_free_bytes",
            "wall_seconds", "owned_processes")
GOLDEN = {
    "start.json": (
        '{"schema": "atx.bounded-research-run/v1", "source_sha": null, "started_utc": "<started_utc>", "command": '
        '["<python>", "-c", "pass"], "executable_sha256": "<executable_sha256>", "argv_sha256": '
        '"07dd796ac46fae391baf8111f8dd37438015d4f5917f1653568d66d65b2b6e46", "attempt": 1, "build_type": null, '
        '"bindings": [], "limits": {"seconds": 60.0, "max_rss_mib": 512, "min_free_mib": 64}, '
        '"sampled_peak_tree_rss_bytes": "<sampled_peak_tree_rss_bytes>", "minimum_system_free_bytes": '
        '"<minimum_system_free_bytes>", "outcome": "launch-failed", "exit_code": null, "git": "none (--no-git: root '
        'outside any repository)", "dirty_outside_pathspec": []}'),
    "receipt.json": (
        '{"schema": "atx.bounded-research-run/v1", "source_sha": null, "started_utc": "<started_utc>", "command": '
        '["<python>", "-c", "pass"], "executable_sha256": "<executable_sha256>", "argv_sha256": '
        '"07dd796ac46fae391baf8111f8dd37438015d4f5917f1653568d66d65b2b6e46", "attempt": 1, "build_type": null, '
        '"bindings": [], "limits": {"seconds": 60.0, "max_rss_mib": 512, "min_free_mib": 64}, '
        '"sampled_peak_tree_rss_bytes": "<sampled_peak_tree_rss_bytes>", "minimum_system_free_bytes": '
        '"<minimum_system_free_bytes>", "outcome": "completed", "exit_code": 0, "git": "none (--no-git: root '
        'outside any repository)", "dirty_outside_pathspec": [], "wall_seconds": "<wall_seconds>", '
        '"owned_processes": "<owned_processes>", "ownership_scope": "sampled descendant identities; commands must '
        'not detach unsampled children", "logs": {"stdout.log": '
        '"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "stderr.log": '
        '"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}}'),
}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


@pytest.fixture(autouse=True)
def fresh_hook():
    """The hook decides once per process per root: each case starts (and leaves) with nothing decided."""
    H._catalogs.clear()
    H._registry = None
    yield
    H._catalogs.clear()
    H._registry = None


def catalog_on(root: Path, exe: bytes = b"not an executable") -> Path:
    """``root`` with the two files that turn the hook on (an empty catalog file and an exe of the given bytes)."""
    for rel, data in ((H.CATALOG_REL, b""), (H.EXE_REL, exe)):
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_bytes(data)
    return root


def normalized(doc: dict) -> dict:
    out = {k: (f"<{k}>" if k in VOLATILE else v) for k, v in doc.items()}
    out["command"] = ["<python>", *doc["command"][1:]]
    return out


def rendered(doc: dict) -> bytes:
    """The runner's bytes for ``doc``: json.dumps(indent=2) + newline, written in text mode (the host's line end)."""
    return (json.dumps(doc, indent=2) + "\n").replace("\n", os.linesep).encode("utf-8")


def run_runner(root: Path, out: str, *extra: str) -> subprocess.CompletedProcess:
    argv = [sys.executable, str(RUNNER), "--root", str(root), "--no-git", "--seconds", "60", "--max-rss-mib", "512",
            "--min-free-mib", "64", "--output", str(root / out), "--", sys.executable, "-c", "pass", *extra]
    return subprocess.run(argv, capture_output=True, text=True, timeout=180,
                          env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))


def warnings(err: str) -> list[str]:
    return [line for line in err.splitlines() if line.startswith(WARN)]


class Ingests:
    """A stand-in for the hook's ``subprocess`` module: records every argv and the bytes the exe would read at that
    moment (``files``: the file ``--path`` names under ``--root``, None if absent), exits ``code`` with ``err``."""

    DEVNULL, TimeoutExpired = subprocess.DEVNULL, subprocess.TimeoutExpired

    def __init__(self, code: int = 0, err: str = "", raise_timeout: bool = False):
        self.code, self.err, self.raise_timeout = code, err, raise_timeout
        self.calls: list[list[str]] = []
        self.files: list[bytes | None] = []

    def run(self, argv, **kw):
        self.calls.append(list(argv))
        p = Path(argv[argv.index("--root") + 1]) / argv[argv.index("--path") + 1]
        self.files.append(p.read_bytes() if p.is_file() else None)
        if self.raise_timeout:
            raise subprocess.TimeoutExpired(argv, kw.get("timeout"))
        return subprocess.CompletedProcess(argv, self.code, "", self.err)

    def of(self, flag: str) -> list[str]:
        return [a[a.index(flag) + 1] for a in self.calls]

    def ingested(self) -> list[tuple[str, str]]:
        """(root-relative path, class) per ingest, in call order."""
        return list(zip(self.of("--path"), self.of("--class")))


# ------------------------------------------------------------------ selection (SQL-6)
def test_selection_finds_the_indexes_a_store_could_select(tmp_path):
    root = tmp_path / "r"
    for d in ("work", "cards/store", "fit/abc-train", "fit/def-train", "abs", "plain", "both/inner"):
        (root / d).mkdir(parents=True)
    for rel in ("work/index.sqlite", "cards/index.sqlite", "fit/abc-train/index.sqlite", "abs/index.sqlite",
                "both/index.sqlite", "both/inner/index.sqlite"):
        (root / rel).write_bytes(b"")
    (root / "work" / "a.json").write_text("{}")
    import record_store  # noqa: PLC0415  (on sys.path through wave_context)
    assert H.INDEX_NAME == record_store.INDEX_NAME
    argv = ["work",                       # its own index
            "cards/store",                # no own index: its parent's (record_store.RecordStore)
            "--work-dir=fit",             # --flag=value: a store root's index under it (d/*/)
            str(root / "abs"),            # an absolute token
            "both/inner",                 # its own index wins over its parent's
            "work/a.json", "-c", "pass", "rank(ts_mean(x, 20))", "a|b<c>", "", "missing/dir", "plain"]
    assert H.selection(root, argv) == {
        "schema": "atx.run-store/v1", "catalog": None,
        "indexes": ["abs/index.sqlite", "both/inner/index.sqlite", "cards/index.sqlite",
                    "fit/abc-train/index.sqlite", "work/index.sqlite"]}
    assert H.selection(root, ["-c", "pass", "plain", "work/a.json"]) is None      # nothing found: no block
    assert H.selection(root, ["both"]) == {"schema": H.SCHEMA, "catalog": None,
                                           "indexes": ["both/index.sqlite", "both/inner/index.sqlite"]}
    other = tmp_path / "o"
    catalog_on(other)
    (other / H.EXE_REL).unlink()
    assert H.selection(other, []) is None                                         # the catalog alone: off
    H._catalogs.clear()
    catalog_on(other, b"exe bytes")
    expected = {"schema": H.SCHEMA, "catalog": {"path": H.CATALOG_REL, "exe_sha256": sha(b"exe bytes")},
                "indexes": []}
    assert H.selection(other, []) == expected
    (other / H.CATALOG_REL).unlink()
    assert H.selection(other, []) == expected                                     # decided once per process


# ------------------------------------------------------------------ the bounded runner's receipts
def test_runner_without_catalog_or_index_writes_todays_bytes(tmp_path):
    root = tmp_path / "r"
    root.mkdir()
    done = run_runner(root, "x-run")
    assert done.returncode == 0, done.stderr
    assert "store" not in done.stderr
    for name in ("start.json", "receipt.json"):
        raw = (root / "x-run" / name).read_bytes()
        doc = json.loads(raw)
        assert raw == rendered(doc)                                   # the bytes are the runner's render of doc
        assert json.dumps(normalized(doc)) == GOLDEN[name]            # and doc is the pre-change golden's


def test_runner_records_an_index_and_the_catalog(tmp_path):
    root = tmp_path / "r"
    (root / "work" / "abc-train").mkdir(parents=True)
    (root / "work" / "abc-train" / "index.sqlite").write_bytes(b"")
    done = run_runner(root, "i-run", "work")
    assert done.returncode == 0 and not warnings(done.stderr), done.stderr
    block = {"schema": "atx.run-store/v1", "catalog": None, "indexes": ["work/abc-train/index.sqlite"]}
    for name in ("start.json", "receipt.json"):
        raw = (root / "i-run" / name).read_bytes()
        doc = json.loads(raw)
        assert raw == rendered(doc) and list(doc)[-1] == "store" and doc["store"] == block
        assert list(doc)[:-1] == list(json.loads(GOLDEN[name]))      # every other key where it was
    # the catalog on, with an exe that cannot run: the block names it; each file is one warning; exit unchanged
    catalog_on(root, b"MZ not really")
    done = run_runner(root, "c-run")
    assert done.returncode == 0, done.stderr
    lines = warnings(done.stderr)
    assert [line.split()[2:4] for line in lines] == [["run-start", "c-run/start.json:"],
                                                    ["run-receipt", "c-run/receipt.json:"]]
    assert all("ingest did not start" in line and "repair: atx-research-store catalog --rebuild" in line
               for line in lines)
    receipt = json.loads((root / "c-run" / "receipt.json").read_text())
    assert receipt["outcome"] == "completed" and receipt["exit_code"] == 0
    assert receipt["store"] == {"schema": H.SCHEMA, "catalog": {"path": H.CATALOG_REL,
                                                                "exe_sha256": sha(b"MZ not really")}, "indexes": []}
    assert json.loads(done.stdout) == receipt                         # the printed receipt is the file's
    assert json.loads((root / "c-run" / "start.json").read_text())["store"] == receipt["store"]


def test_hook_off_spawns_and_imports_nothing(tmp_path):
    """No catalog: record() starts no process (subprocess is poisoned in a fresh interpreter: a call would warn) and
    loads neither sqlite3 nor the identity module; the file keeps its bytes."""
    root = tmp_path / "r"
    (root / "w").mkdir(parents=True)
    (root / "w" / "index.sqlite").write_bytes(b"")
    p = root / "a" / "x.json"
    p.parent.mkdir()
    p.write_bytes(b'{"x": 1}\n')
    code = ("import subprocess, sys; sys.path.insert(0, sys.argv[1]); import research_store_hook as H; "
            "subprocess.run = subprocess.Popen = None; "
            "H.record(sys.argv[2], sys.argv[3], 'run-start'); H.record(sys.argv[2], 'a/x.json', 'wave-candidate'); "
            "s = H.selection(sys.argv[2], ['w']); "
            "print(s['catalog'], s['indexes'], 'sqlite3' in sys.modules, 'research_store_identity' in sys.modules)")
    done = subprocess.run([sys.executable, "-c", code, str(HERE.parent), str(root), str(p)], capture_output=True,
                          text=True, timeout=60, env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
    assert done.returncode == 0, done.stderr
    assert done.stderr == ""
    assert done.stdout.split() == ["None", "['w/index.sqlite']", "False", "False"]
    assert p.read_bytes() == b'{"x": 1}\n'


# ------------------------------------------------------------------ the fake wave (tiny world)
def wave_root(path: Path, monkeypatch) -> Path:
    """test_wave_driver.queued_root with time-free digests and fixed git dates; build-equity/ is ignored by its git
    (the catalog files are no part of the tree the wave commits)."""
    monkeypatch.setenv("GIT_AUTHOR_DATE", "2026-10-01T00:00:00+00:00")
    monkeypatch.setenv("GIT_COMMITTER_DATE", "2026-10-01T00:00:00+00:00")
    root = TW.queued_root(path, driver={"receipt_digest": "content", "record_date": "2026-10-01"})
    with (root / ".git" / "info" / "exclude").open("a", encoding="utf-8") as f:
        f.write("build-equity/\n")
    return root


def run_wave(root: Path) -> int:
    return research_wave.main(["run", F.MANIFEST, "--root", str(root)], executor=F.FakeCycle(root, TW.ADMIT_ALL),
                              log=lambda s: None)


GIT_EOL_NOTICE = b"warning: in the working copy of "


def tree_digests(root: Path) -> dict:
    """{root-relative path: digest} of every file but .git and the catalog's own files (JSON: its time-free
    canonical form, stage_chain.time_free; anything else: its bytes without git's CRLF notices, which a commit console
    holds once or twice per file depending on whether git finds the index entry racily clean: timing, not content)."""
    out = {}
    for p in sorted(root.rglob("*")):
        rel = p.relative_to(root).as_posix()
        if not p.is_file() or rel.split("/")[0] == ".git" or rel in (H.CATALOG_REL, H.EXE_REL):
            continue
        data = p.read_bytes()
        if p.suffix == ".json":
            data = SC.canonical(SC.time_free(json.loads(data))).encode("utf-8")
        else:
            data = b"\n".join(line for line in data.split(b"\n") if not line.startswith(GIT_EOL_NOTICE))
        out[rel] = sha(data)
    return out


def remove_tree(path: Path) -> None:
    def writable(func, p, _exc):                            # git's read-only object files on Windows
        os.chmod(p, stat.S_IWRITE)
        func(p)
    shutil.rmtree(path, onexc=writable)


def fake_store(root: Path, monkeypatch) -> tuple[Ingests, list]:
    """The hook on for ``root`` with a fake exe and a fake check: (the ingests, the (path, class) of each check)."""
    catalog_on(root)
    H._catalogs.clear()                                     # a write before this one may have decided "off"
    fake, checked = Ingests(), []
    monkeypatch.setattr(H, "subprocess", fake)
    monkeypatch.setattr(H, "check", lambda r, rel, cls, catalog: checked.append((rel, cls)))
    return fake, checked


def test_one_ingest_per_write_in_a_fake_wave(tmp_path, monkeypatch):
    root = wave_root(tmp_path / "w", monkeypatch)
    fake, checked = fake_store(root, monkeypatch)
    assert run_wave(root) == 0
    receipts = sorted(f"out/waves/w1/receipts/{p.name}" for p in (root / "out/waves/w1/receipts").glob("*.json"))
    candidates = [f"scripts/specs/v8/candidates/{c}.json" for c in ("alpha_a", "alpha_b", "alpha_c")]
    result = "out/waves/w1/wave-result.json"                # item 5: wave_stage_record.record's wave result
    assert len(receipts) == 9
    assert sorted(fake.ingested()) == sorted([(r, "stage-receipt") for r in receipts]
                                             + [(c, "wave-candidate") for c in candidates] + [(result, "wave-result")])
    assert fake.files[fake.ingested().index((result, "wave-result"))] == (root / result).read_bytes()  # after the write
    assert [a[:2] for a in fake.calls] == [[str(root.resolve() / H.EXE_REL), "ingest"]] * len(fake.calls)
    assert set(fake.of("--root")) == {str(root.resolve())}
    assert set(fake.of("--catalog")) == {str(root.resolve() / H.CATALOG_REL)}
    assert set(fake.of("--classes")) == {str(H.CLASSES)}
    assert sorted(checked) == sorted(zip(fake.of("--path"), fake.of("--class")))   # each ingest, then its check
    calls = len(fake.calls)
    assert run_wave(root) == 0 and len(fake.calls) == calls                        # all done: nothing written


def test_fake_wave_bytes_equal_with_a_failing_store(tmp_path, monkeypatch, capsys):
    path = tmp_path / "w"
    root = wave_root(path, monkeypatch)
    assert run_wave(root) == 0
    plain = tree_digests(root)
    assert not warnings(capsys.readouterr().err)
    remove_tree(path)
    root = wave_root(path, monkeypatch)                     # the same root path: every recorded path is the same
    catalog_on(root, b"MZ cannot start")
    H._catalogs.clear()
    assert run_wave(root) == 0
    assert tree_digests(root) == plain
    lines = warnings(capsys.readouterr().err)
    classes = [line.split()[2] for line in lines]
    assert classes.count("stage-receipt") == 9 and classes.count("wave-candidate") == 3
    assert classes.count("wave-result") == 1 and len(classes) == 13                  # item 5: the wave result
    assert all("ingest did not start" in line for line in lines)
    # a stage that fails: the same files, failed receipt included, and exit code with and without the store
    seen = {}
    for on in (False, True):
        if path.exists():
            remove_tree(path)
        H._catalogs.clear()                                     # a new tree at the path: decided anew
        root = wave_root(path, monkeypatch)
        if on:
            catalog_on(root, b"MZ cannot start")
        H._catalogs.clear()
        code = research_wave.main(["run", F.MANIFEST, "--root", str(root)],
                                  executor=F.FakeCycle(root, TW.ADMIT_ALL, fail={"--screen": 7}), log=lambda s: None)
        assert len(list((root / "out/waves/w1/receipts").glob("*.failed-*.json"))) == 1
        seen[on] = (code, tree_digests(root), len(warnings(capsys.readouterr().err)))
    assert seen[False][0] == seen[True][0] != 0 and seen[False][1] == seen[True][1]
    assert seen[False][2] == 0 and seen[True][2] == 3            # preflight, register ok; screen failed


# ------------------------------------------------------------------ item 5: the cycle's record writers (fake store)
BINDING_STEP = {"output": "out/nav-ref", "phase": "ref", "run_dir": "out/nav-ref-run1",
                "argv": ["py", "run_bounded_research.py", "--", "x.exe", "nav", "--output", "o"]}
VERDICT_DOC = {"schema": cycle_verdict.VERDICT_SCHEMA, "cycle": "c1", "mode": "run", "spec_sha256": "c" * 64,
               "admission": [{"id": "a", "admitted": True}], "marginal": [],
               "phases": [{"name": "nav", "seconds": 1.5, "peak_mib": 50}],
               "paired": {"dsr": 0.5, "se": 0.25, "cbb_ci": [0.125, 0.875], "lw_p": 1e-300},
               "ledger": {"path": "out/trials.jsonl", "head": "e" * 64, "lines": 5}}


def cycle_root(tmp_path: Path, monkeypatch, relative: bool) -> tuple[Path, Resolver]:
    """A root and the cycle's Resolver of it: absolute, or relative to the cwd as ``research_cycle.py --root r`` takes
    it (a cwd-relative path handed to the hook would name <root>/<root>/...: the calls pass root-relative names)."""
    root = tmp_path / "r"
    root.mkdir()
    monkeypatch.chdir(tmp_path)
    return root, Resolver(Path("r") if relative else root)


@pytest.mark.parametrize("relative", [False, True], ids=["absolute-root", "relative-root"])
def test_write_binding_ingests_its_binding(tmp_path, monkeypatch, relative):
    root, res = cycle_root(tmp_path, monkeypatch, relative)
    fake, checked = fake_store(root, monkeypatch)
    st = SimpleNamespace(**BINDING_STEP)
    (root / st.run_dir).mkdir(parents=True)
    doc = cycle_resume.write_binding(SimpleNamespace(res=res, spec_path=None), st)
    rel = f"{st.run_dir}/{cycle_resume.BINDING}"
    data = (root / rel).read_bytes()
    assert data == rendered(doc)                                        # the writer's bytes (host text mode)
    assert fake.ingested() == checked == [(rel, "cycle-binding")]
    assert fake.of("--root") == [str(root.resolve())] and fake.files == [data]     # after the write: the whole file


@pytest.mark.parametrize("keep", [False, True], ids=["no-copy", "keep-verdicts"])
def test_write_verdict_ingests_the_verdict_then_its_copy(tmp_path, monkeypatch, keep):
    root, res = cycle_root(tmp_path, monkeypatch, relative=True)
    fake, checked = fake_store(root, monkeypatch)
    monkeypatch.setattr(cycle_verdict, "verdict", lambda cycle, timings, spec_sha256, ledger: dict(VERDICT_DOC))
    cycle = SimpleNamespace(res=res, cycle_dir=lambda: "out/cycle-c1")
    assert cycle_verdict.write_verdict(cycle, {}, None, lambda s: None, keep=keep) == VERDICT_DOC
    rels = ["out/cycle-c1/cycle_verdict.json"] + (["out/cycle-c1/verdicts/run-1.json"] if keep else [])
    text = (json.dumps(VERDICT_DOC, indent=2) + "\n").encode("utf-8")
    assert fake.ingested() == checked == [(rel, "cycle-verdict") for rel in rels]
    assert fake.files == [text] * len(rels) and [(root / rel).read_bytes() for rel in rels] == [text] * len(rels)


def test_keep_copy_ingests_each_copy(tmp_path, monkeypatch):
    root, res = cycle_root(tmp_path, monkeypatch, relative=True)
    fake, checked = fake_store(root, monkeypatch)
    cycle = SimpleNamespace(res=res, cycle_dir=lambda: "out/cycle-c1")
    texts = ['{"n": 1}\n', '{"n": 2}\n']
    copies = [cycle_verdict.keep_copy(cycle, "run", text) for text in texts]   # the second finds run-1 taken
    assert copies == ["out/cycle-c1/verdicts/run-1.json", "out/cycle-c1/verdicts/run-2.json"]
    assert fake.ingested() == checked == [(c, "cycle-verdict") for c in copies]
    assert fake.files == [text.encode("utf-8") for text in texts]


# ------------------------------------------------------------------ failures are one warning each
def test_failures_are_one_warning_each(tmp_path, monkeypatch, capsys):
    root = catalog_on(tmp_path / "r")
    doc = dict(F.candidate("alpha_a"), schema=WQ.SCHEMA, status="proposed", wave=None,
               history=[{"status": "proposed", "at": "2026-10-02", "by": "lane"}])
    WQ.write(tmp_path / "plain", doc)                                   # the bytes of a write without the hook
    expect = (tmp_path / "plain" / WQ.QUEUE_DIR / "alpha_a.json").read_bytes()
    rel = f"{WQ.QUEUE_DIR}/alpha_a.json"
    refused = Ingests(3, "atx-research-store: candidate alpha_a is claimed by another path\n")
    monkeypatch.setattr(H, "subprocess", refused)
    assert WQ.write(root, doc) == root / WQ.QUEUE_DIR / "alpha_a.json"   # the writer's return value unchanged
    assert (root / rel).read_bytes() == expect
    assert warnings(capsys.readouterr().err) == [
        f"{WARN}wave-candidate {rel}: ingest exit 3 (atx-research-store: candidate alpha_a is claimed by another "
        "path) (the JSON stands; repair: atx-research-store catalog --rebuild)"]
    monkeypatch.setattr(H, "subprocess", Ingests(raise_timeout=True))
    H.record(root, rel, "wave-candidate")
    assert [line.split(f"{rel}: ", 1)[1] for line in warnings(capsys.readouterr().err)] == [
        "ingest timed out after 60 s (the JSON stands; repair: atx-research-store catalog --rebuild)"]
    # ingest ok, the rows do not re-render the file: the checker's reason, one line
    monkeypatch.setattr(H, "subprocess", Ingests())
    mismatch = SimpleNamespace(reason="rendered sha256 aa != catalogued bb")
    stub = SimpleNamespace(load_registry=lambda path: {"wave-candidate": {"render": {"mode": "doc"}}},
                           check_one=lambda db, rel, cls, root=None, registry=None: mismatch)
    monkeypatch.setattr(H, "identity", lambda: stub)
    H.record(root, root / rel, "wave-candidate")
    assert [line.split(f"{rel}: ", 1)[1] for line in warnings(capsys.readouterr().err)] == [
        "rendered sha256 aa != catalogued bb (the JSON stands; repair: atx-research-store catalog --rebuild)"]
    H.record(root, tmp_path / "elsewhere.json", "wave-candidate")      # outside the root: named, not ingested
    assert "outside the root" in capsys.readouterr().err


# ------------------------------------------------------------------ the real store exe
@pytest.mark.skipif(not EXE.is_file(), reason=f"{EXE} not built (target atx-research-store)")
def test_real_store_exe_ingests_every_class(tmp_path, monkeypatch, capsys):
    import research_store_identity as rsi  # noqa: PLC0415
    import wave_context  # noqa: PLC0415
    root = tmp_path / "r"
    (root / H.EXE_REL).parent.mkdir(parents=True)
    shutil.copy2(EXE, root / H.EXE_REL)
    (root / H.CATALOG_REL).parent.mkdir(parents=True)
    db = root / H.CATALOG_REL
    init = subprocess.run([str(root / H.EXE_REL), "init", "--catalog", str(db)], capture_output=True, text=True,
                          timeout=60)
    assert init.returncode == 0, init.stderr
    written: list[tuple[str, str]] = []

    # the runner: start.json and receipt.json, each ingested by the runner's own hook
    done = run_runner(root, "nav-run")
    assert done.returncode == 0 and not warnings(done.stderr), done.stderr
    receipt = json.loads((root / "nav-run" / "receipt.json").read_text())
    assert receipt["store"]["catalog"] == {"path": H.CATALOG_REL, "exe_sha256": sha(EXE.read_bytes())}
    written += [("nav-run/start.json", "run-start"), ("nav-run/receipt.json", "run-receipt")]

    # stage receipts (ok and failed) through a chain wired as research_wave.chain_of wires it
    def stage(name, fail=False):
        def run(ctx, done_, log):
            if fail:
                raise SC.StageError(f"{name} refused", 4)
            return {"value": name}
        return SC.Stage(name, run)
    state = root / "out" / "waves" / "w1"
    chain = SC.Chain("atx.research-wave", [stage("preflight"), stage("register", fail=True)], state,
                     digest="content", on_write=lambda p: H.record(root, p, "stage-receipt"))
    with pytest.raises(SC.ChainError):
        chain.run(None, log=lambda s: None)
    written += [("out/waves/w1/receipts/01-preflight.json", "stage-receipt"),
                ("out/waves/w1/receipts/02-register.failed-1.json", "stage-receipt")]

    # candidates (the queue's write) and an emitted manifest (candidates emit)
    for cid in ("alpha_a", "alpha_b"):
        doc = dict(F.candidate(cid, citation="Synthetic (2026) \u2014 \U0001F600"), schema=WQ.SCHEMA,
                   status="proposed", wave=None, history=[{"status": "proposed", "at": "2026-10-02", "by": "lane"}])
        WQ.write(root, WQ.transition(doc, "pinned", "PM", "2026-10-02"))
        written.append((f"{WQ.QUEUE_DIR}/{cid}.json", "wave-candidate"))
    head = {k: v for k, v in TW.manifest().items() if k != "candidates"}
    (root / "head.json").write_text(json.dumps(head))
    assert WQ.main(["emit", "--root", str(root), "--head", str(root / "head.json"), "--select", "alpha_a,alpha_b",
                    "--output", "scripts/specs/v8/waves/w2.json"]) == 0
    written.append(("scripts/specs/v8/waves/w2.json", "wave-manifest"))

    # the writers of item 5, each ingesting by its own call: the binding (write_binding), the verdict and its copy
    # (write_verdict, keep on, its doc fixed); the wave result as wave_stage_record.record writes it (Wave.write_json),
    # then the call that writer makes
    res = Resolver(root)
    st = SimpleNamespace(**BINDING_STEP)
    (root / st.run_dir).mkdir(parents=True)
    cycle_resume.write_binding(SimpleNamespace(res=res, spec_path=None), st)
    written.append((f"{st.run_dir}/cycle_binding.json", "cycle-binding"))
    monkeypatch.setattr(cycle_verdict, "verdict", lambda cycle, timings, spec_sha256, ledger: dict(VERDICT_DOC))
    cycle_verdict.write_verdict(SimpleNamespace(res=res, cycle_dir=lambda: "out/cycle-c1"), {}, None, lambda s: None,
                                keep=True)
    written += [("out/cycle-c1/cycle_verdict.json", "cycle-verdict"),
                ("out/cycle-c1/verdicts/run-1.json", "cycle-verdict")]
    wave_context.Wave.write_json(res, "out/waves/w1/wave-result.json", {
        "schema": "atx.wave-result/v1", "wave": "w1", "kind": "rule",
        "manifest": {"path": "scripts/specs/v8/waves/w1.json", "sha256": "1" * 64, "commit": None},
        "stats": {"cell": {"placeholder": 1.0, "big": 1e16, "small": 5e-324}},
        "verdict": {"accepted": False, "reason": "placeholder"},
        "ledger": {"path": "out/trials.jsonl", "head": "e" * 64, "n_before": 3, "n_after": 4, "trial_id": "t5"},
        "next_parent": {"spec": "scripts/specs/v8/w1.json", "library": "lib"},
        "timings": [{"phase": "nav", "seconds": 12.345678901234567, "peak_mib": 50}]})
    H.record(root, res.path("out/waves/w1/wave-result.json"), "wave-result")
    written.append(("out/waves/w1/wave-result.json", "wave-result"))

    assert not warnings(capsys.readouterr().err)                       # every ingest and every check passed
    for rel, cls in written:
        if cls != "wave-manifest":
            assert rsi.check_one(db, rel, cls, root=root) is None, rel
    out = io.StringIO()
    assert rsi.main(["--catalog", str(db), "--root", str(root)], out=out) == rsi.EXIT_OK, out.getvalue()
    assert out.getvalue().endswith(f"total checked {len(written) - 1} mismatches 0\n"), out.getvalue()

    # a file whose bytes the writer's rule does not re-render: ingested, then one warning naming the render
    (root / "bad-run").mkdir()
    (root / "bad-run" / "start.json").write_text(json.dumps({"schema": "atx.bounded-research-run/v1"}) + "\n",
                                                 encoding="utf-8", newline="\n")
    H.record(root, "bad-run/start.json", "run-start")
    lines = warnings(capsys.readouterr().err)
    assert len(lines) == 1 and lines[0].startswith(f"{WARN}run-start bad-run/start.json: rendered sha256 ")
    # N3 (ruling W3-4: candidates stay keyed by id): the same id in a second queue dir is refused, one warning
    doc = json.loads((root / WQ.QUEUE_DIR / "alpha_a.json").read_text())
    WQ.write(root, doc, "scripts/specs/p9/candidates")
    lines = warnings(capsys.readouterr().err)
    assert len(lines) == 1 and lines[0].startswith(f"{WARN}wave-candidate scripts/specs/p9/candidates/alpha_a.json: "
                                                   "ingest exit 3")
    assert lines[0].endswith("(the JSON stands; repair: atx-research-store catalog --rebuild)")
