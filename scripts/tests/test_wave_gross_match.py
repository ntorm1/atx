"""The gross-match retirement of P9 lane C3b (rulings C2-9, C2-10, C2-11, C3-12, E2-16a, PRE-27): the Python match
mode pm6-6 is gone, pm6-6-engine (PM6-6 inside the NAV exe) is the only gross match. Synthetic worlds only
(wave_fixture.py's fake root and executor; the committed v2 kinds fixtures re-rooted as test_wave_kinds does).

  "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_wave_gross_match.py

  test_ledgered_v1_manifests_still_validate            the v8 wave manifests naming pm6-6 load as before (C2-11)
  test_pm6_6_is_refused_at_preflight_naming_the_engine  a real v2 fixture and a v1 manifest at pm6-6: exit 3 at
                                                        preflight, before register; plan and status still work
  test_a_state_dir_older_than_the_retirement_is_refused  run and match refuse pm6-6 themselves (a preflight receipt
                                                        written before the retirement)
  test_match_fails_closed_on_a_mode_without_a_stage     a live mode that is neither none nor engine has no match stage
  test_a_manifest_without_gross_match_is_refused_at_plan  ruling C2-9: no default; the message names the value
  test_a_cell_edited_during_judge_writes_no_wave_result  rulings C2-10 / C3-12: wave_result.build's digest re-check
  test_the_reference_keeps_its_leverage_across_the_match  ruling E2-16a: ref.leverage stays the pre-calibration L
  test_the_next_parent_is_the_plain_record_never_the_run_recipe  ruling PRE-27: a v2 wave on wave 1's -gm calibrates
                                                        at L'; on its -cal it is refused at preflight (E2-14 R3a)
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import research_cycle as RC  # noqa: E402
import research_wave  # noqa: E402
import test_wave_kinds as K  # noqa: E402  (lane E2's kinds helpers: fixture loader, fake world, KindsFake)
import wave_fixture as F  # noqa: E402
import wave_manifest as WM  # noqa: E402
import wave_rules as WR  # noqa: E402
import wave_stage_cell as WSC  # noqa: E402
from wave_context import Wave  # noqa: E402
from wave_stage_util import EXIT_PIN, StageError  # noqa: E402

REPO = HERE.parents[1]
V1_WAVES = REPO / "scripts" / "specs" / "v8" / "waves"
KEPT_ALL = {"alpha_a": ("admitted", 1), "alpha_b": ("admitted", 0), "alpha_c": ("reject_redundant", 0)}
ONE_DROPPED = {"alpha_a": ("admitted", 1), "alpha_b": ("admitted", -1), "alpha_c": ("admitted", 1)}
RETIRED = 'gross_match "pm6-6" is retired (ruling C2-11'


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run_wave(root: Path, fake, *args: str, manifest: str = F.MANIFEST) -> tuple[int, list[str]]:
    log: list[str] = []
    code = research_wave.main([args[0], manifest, "--root", str(root), *args[1:]], executor=fake, log=log.append)
    return code, log


def receipts(root: Path, wave: str = "w1") -> Path:
    return root / "out/waves" / wave / "receipts"


# ------------------------------------------------------------------ C2-11: the retired name
def test_ledgered_v1_manifests_still_validate():
    """The ledgered v8 wave manifests name pm6-6 (their results are ledgered; never edited): they validate as before, so
    plan and status of their state dirs keep working, and the retired name maps to the engine mode for the refusal."""
    names = []
    for p in sorted(V1_WAVES.glob("*.json")):
        m = json.loads(p.read_text(encoding="utf-8"))
        got = WM.validate(m)
        assert got == ([] if p.name != "y-s.head.json" else
                       ["a wave has exactly one of candidates (a library wave) and rule_cell (a rule wave)"]), p.name
        names.append(m["gross_match"])
    assert set(names) == {"pm6-6"} and len(names) == 5
    assert WR.RETIRED_GROSS_MATCH == {"pm6-6": "pm6-6-engine"} and "pm6-6" not in WR.GROSS_MATCH
    assert '"pm6-6-engine"' in WR.retired_gross_match("pm6-6") and "C2-9" in WR.retired_gross_match("pm6-6")
    assert WR.retired_gross_match("pm6-6-engine") is None and WR.retired_gross_match("none") is None
    assert WR.retired_gross_match(None) is None
    assert WR.validate_names(None, "pm7-34", [], "pm6-7") == ["gross_match 'pm6-7' is not one of none, pm6-6-engine"]


def test_pm6_6_is_refused_at_preflight_naming_the_engine(tmp_path):
    """Ruling C2-11: a manifest naming the retired Python match is refused at preflight (exit 3, nothing registered),
    naming pm6-6-engine; on a real v2 kinds fixture and on a v1 library wave. plan and status still exit 0 (the
    manifest loads), plan printing the refusal where the run and match stages would stop."""
    m = K.load_fixture("new-signal.json")
    m["gross_match"] = "pm6-6"
    root = K.world(tmp_path / "v2", m)
    code, log = run_wave(root, K.KindsFake(root), "run", "--until", "preflight")
    assert code == EXIT_PIN, log
    err = K.preflight_error(root, m)
    assert RETIRED in err and 'names "pm6-6-engine"' in err, err
    assert not (receipts(root, m["wave"]) / "02-register.json").exists()
    root = F.build(tmp_path / "v1", gross_match="pm6-6")
    fake = F.FakeCycle(root, KEPT_ALL)
    code, log = run_wave(root, fake, "plan")
    assert code == 0 and fake.calls == []
    refused = [x for x in log if x.startswith("#   REFUSED (exit 3): " + RETIRED)]
    assert len(refused) == 2, log                                       # the run and the match stage's plans
    code, log = run_wave(root, fake, "run")
    assert code == EXIT_PIN and RETIRED in json.loads((receipts(root) / "01-preflight.failed-1.json").read_text())[
        "error"]
    assert not (receipts(root) / "02-register.json").exists() and not [c for c in fake.calls if c[0] != "git"]
    assert run_wave(root, fake, "status")[0] == 0


@pytest.mark.parametrize("stage", ["run", "match"])
def test_a_state_dir_older_than_the_retirement_is_refused(tmp_path, monkeypatch, stage):
    """A state dir whose stages before ``stage`` passed while pm6-6 was live (simulated: the retirement switched off
    for those stages) stops at ``stage`` itself, exit 3, naming pm6-6-engine: no NAV runs, no receipt is written."""
    root = F.build(tmp_path / "r", gross_match="pm6-6")
    fake = F.FakeCycle(root, KEPT_ALL)
    before = {"run": "spec", "match": "run"}[stage]
    with monkeypatch.context() as mp:
        mp.setattr(WR, "retired_gross_match", lambda mode: None)
        mp.setattr(WR, "GROSS_MATCH", dict(WR.GROSS_MATCH, **{"pm6-6": None}))   # as "none" at the run stage
        code, log = run_wave(root, fake, "run", "--until", before)
        assert code == 0, log
    calls = len(fake.calls)
    code, log = run_wave(root, fake, "run")
    n = {"run": "05", "match": "06"}[stage]
    err = json.loads((receipts(root) / f"{n}-{stage}.failed-1.json").read_text())["error"]
    assert code == EXIT_PIN and err.startswith(f"{stage}: {RETIRED}") and 'names "pm6-6-engine"' in err, err
    assert not (receipts(root) / f"{n}-{stage}.json").exists()
    assert not [c for c in fake.calls[calls:] if c[0] != "git"]       # nothing ran (git status / log queries only)


def test_match_fails_closed_on_a_mode_without_a_stage(tmp_path, monkeypatch):
    """A live gross_match mode that is neither none nor engine (a GROSS_MATCH row added without its stage) is refused
    by the match stage, exit 3, never matched as none."""
    root = F.build(tmp_path / "r", gross_match="none")
    monkeypatch.setattr(WR, "GROSS_MATCH", dict(WR.GROSS_MATCH, **{"pm6-6-x": {"tolerance": 0.005}}))
    w = Wave(F.MANIFEST, root, executor=F.FakeCycle(root, KEPT_ALL), log=lambda s: None)
    w.manifest = dict(w.manifest, gross_match="pm6-6-x")
    with pytest.raises(StageError, match="match: gross_match 'pm6-6-x' is neither none nor an engine mode") as e:
        WSC.match(w, {"spec": {"cell_spec": "scripts/specs/v8/lib-w1.json"}}, lambda s: None)
    assert e.value.code == EXIT_PIN


# ------------------------------------------------------------------ C2-9: no default
def test_a_manifest_without_gross_match_is_refused_at_plan(tmp_path, capsys):
    """Ruling C2-9: gross_match has no default. Absent (or null) it is refused at load, plan exit 2, by one message that
    names the key and the value the wave needs: "pm6-6-engine", or "none" for a v2 kind leverage."""
    want = 'gross_match is missing: the wave names "pm6-6-engine"'
    for m in (F.manifest(drop=("gross_match",)), F.manifest(gross_match=None)):
        m["fields"]["manifest_sha256"] = "0" * 64
        got = WM.validate(m)
        assert len(got) == 1 and got[0].startswith(want) and "C2-9" in got[0], got
    for name in ("new-signal.json", "new-horizon.json"):
        m = K.load_fixture(name)
        del m["gross_match"]
        assert [p for p in WM.validate(m) if p.startswith("gross_match")] == [WM.gross_match_missing(m)], name
        assert WM.gross_match_missing(m).startswith(want)
    m = K.load_fixture("leverage-change.json")
    del m["gross_match"]
    assert 'gross_match is missing: a kind leverage wave names "none" (ruling C2-9' in "; ".join(WM.validate(m))
    root = F.build(tmp_path / "r", drop=("gross_match",))
    fake = F.FakeCycle(root, KEPT_ALL)
    capsys.readouterr()
    code, _ = run_wave(root, fake, "plan")
    err = capsys.readouterr().err
    assert code == 2 and want in err and "missing ['gross_match']" not in err and fake.calls == [], err


# ------------------------------------------------------------------ C2-10 / C3-12: the digest re-check, then the result
class EditDuringJudge(F.FakeCycle):
    """Review S14: the judge's research_cycle run (no --stop-after, no --screen) commits an edit of ``target(spec)``
    (None: no edit) after it ran, inside the same chain run: verify's own input pins already passed."""

    def __init__(self, root: Path, admission: dict, target, **kw):
        super().__init__(root, admission, **kw)
        self.target, self.edited = target, []

    def cycle(self, args):
        done = super().cycle(args)
        if args[0] == "run" and "--stop-after" not in args and "--screen" not in args and self.target:
            rel = self.target(args[1])
            doc = json.loads((self.root / rel).read_text())
            doc["nav"]["leverage"] = "1.9999"
            F.write_json(self.root, rel, doc)
            F.git(self.root, "commit", "-q", "-am", f"edit {rel} during the judge")
            self.edited.append(rel)
        return done


@pytest.mark.parametrize("mode, target, key", [
    ("pm6-6-engine", None, None),                                                      # the control: unedited
    ("pm6-6-engine", lambda spec: spec.replace("-cal.json", "-gm.json"), "spec"),      # the cell of record
    ("pm6-6-engine", lambda spec: spec, "run_spec"),                                   # the run recipe that ran
    ("none", lambda spec: spec, "spec"),                                               # gross_match none: the cell
], ids=["control", "record-spec", "run-recipe", "none-cell"])
def test_a_cell_edited_during_judge_writes_no_wave_result(tmp_path, mode, target, key):
    """Rulings C2-10 (S14) and C3-12: wave_result.build re-checks the digests of the cell's spec files first; a file
    committed with other content after the match stage recorded it stops the record stage, exit 3, naming the file,
    and no wave-result.json, log section or ledger-head copy is written. Unedited, the wave records its result."""
    root = F.build(tmp_path / "r", gross_match=mode)
    fake = EditDuringJudge(root, KEPT_ALL, target)
    code, log = run_wave(root, fake, "run")
    state = root / "out/waves/w1"
    if target is None:
        assert code == 0, log
        cell = json.loads((state / "wave-result.json").read_text())["cell"]
        assert cell["spec_digest"] == F.research_spec.spec_digest(root / cell["spec"], F.research_tree.REPO)
        return
    assert code == EXIT_PIN and len(fake.edited) == 1, log
    err = json.loads((state / "receipts/09-record.failed-1.json").read_text())["error"]
    assert err.startswith("record: the cell's spec changed after the match stage (ruling C2-10): ")
    assert f"{key} {fake.edited[0]} has digest " in err and "no wave result is written" in err, err
    assert not (state / "wave-result.json").exists() and not (state / "wave-log.md").exists()
    assert not (state / "ledger-head.json").exists() and not (root / "sprint/waves/w1").exists()


# ------------------------------------------------------------------ E2-16a: the reference across the match stage
def test_the_reference_keeps_its_leverage_across_the_match(tmp_path):
    """Ruling E2-16a: by PM6-6 only the cell is calibrated. A v2 rule cell whose template sets its own ref block
    (the new-combination-rule fixture, the block re-pinned as test_wave_kinds does), its gross mismatched, run to
    verify: the run recipe -cal and the cell of record -gm both resolve ref.leverage to the pre-calibration L 1.1474
    while the record's nav.leverage is the published L'; verify passes on that cell of record."""
    m = K.load_fixture("new-combination-rule.json")
    root = K.world(tmp_path / "w", m)
    K.set_template(root, m, K.tpl_set(ref=K.REF_OK))
    assert run_wave(root, K.KindsFake(root), "run", "--until", "spec")[0] == 0
    cell = K.cell_of(root)["cell_spec"]
    name = RC.load_spec(root / cell)["name"]
    fake = K.KindsFake(root, gross_per_l={name: 0.9604 / 1.1474})                        # one correction
    code, log = run_wave(root, fake, "run", "--until", "verify")
    assert code == 0, log
    mt = json.loads((receipts(root, m["wave"]) / "06-match.json").read_text())["outputs"]
    assert mt["engine"] is True and mt["corrected"] is True and mt["leverage"] not in ("1.1474", "1.9999")
    record, ran = RC.load_spec(root / mt["record_spec"]), RC.load_spec(root / mt["cell_spec"])
    assert record["ref"]["leverage"] == ran["ref"]["leverage"] == "1.1474"
    assert record["ref"]["output"] == ran["ref"]["output"] == RC.load_spec(root / cell)["ref"]["output"]
    assert record["nav"]["leverage"] == mt["leverage"] and ran["nav"]["leverage"] == "1.1474"
    assert (receipts(root, m["wave"]) / "07-verify.json").is_file()


# ------------------------------------------------------------------ PRE-27: the parent after a calibrated cell
def child_manifest(root: Path, parent: dict, wave: str) -> str:
    """The new-horizon kinds fixture as wave ``wave`` on ``parent`` (wave 1's next_parent or its run recipe), committed
    beside wave 1's manifest; the fake world's fields pin, ledger and N after wave 1."""
    m = K.load_fixture("new-horizon.json")
    m.update(wave=wave, parent=parent, ledger=F.LEDGER, expect={"n_before": 3}, out_dir=f"out/waves/{wave}",
             fields={"dir": F.FIELDS, "manifest_sha256": sha((root / F.FIELDS / "manifest.json").read_bytes())})
    rel = f"scripts/specs/v8/waves/{wave}.json"
    F.write_json(root, rel, m)
    F.git(root, "add", "-A")
    F.git(root, "commit", "-q", "-m", f"wave {wave} manifest")
    return rel


def test_the_next_parent_is_the_plain_record_never_the_run_recipe(tmp_path):
    """Ruling PRE-27: wave 1 (engine, accepted, one correction to L' 1.1782) names its plain cell of record
    lib-w1b-gm.json as next_parent. A v2 wave on that parent passes preflight and its match stage calibrates at the
    parent's L' (the run recipe -cal at L 1.1782, one --calibrate-gross). The same wave on the run recipe -cal (which
    carries --calibrate-gross) is refused at preflight, exit 3, before register (ruling E2-14, R3a)."""
    root = F.build(tmp_path / "r")
    for t in (K.KINDS_DIR / "templates").glob("*.json"):                 # the kinds templates, as K.world commits them
        (root / t.relative_to(REPO)).parent.mkdir(parents=True, exist_ok=True)
        (root / t.relative_to(REPO)).write_bytes(t.read_bytes())
    F.git(root, "add", "-A")
    F.git(root, "commit", "-q", "-m", "kinds templates")
    fake = F.FakeCycle(root, ONE_DROPPED, gross_per_l={"w1b": 0.9604 / 1.1474}, dsr=0.05)
    assert run_wave(root, fake, "run")[0] == 0
    res = json.loads((root / "out/waves/w1/wave-result.json").read_text())
    nxt, run_spec = res["next_parent"], res["cell"]["run_spec"]
    assert res["verdict"]["accepted"] is True and nxt == {"spec": "scripts/specs/v8/lib-w1b-gm.json", "library": "w1b"}
    assert res["cell"]["leverage"] == "1.1782" and run_spec == "scripts/specs/v8/lib-w1b-cal.json"
    rel = child_manifest(root, {"spec": run_spec, "library": "w1b"}, "k-cal")
    code, log = run_wave(root, K.KindsFake(root), "run", "--until", "preflight", manifest=rel)
    assert code == EXIT_PIN, log
    err = json.loads((receipts(root, "k-cal") / "01-preflight.failed-1.json").read_text())["error"]
    assert f"the parent spec {run_spec} nav.flags (the cell inherits it): the cell's nav.flags carries " \
           "['--calibrate-gross', '--calibrate-tolerance']" in err and "ruling E2-14" in err, err
    assert not (receipts(root, "k-cal") / "02-register.json").exists()
    rel = child_manifest(root, nxt, "k-gm")
    code, log = run_wave(root, K.KindsFake(root), "run", "--until", "match", manifest=rel)
    assert code == 0, log
    pre = json.loads((receipts(root, "k-gm") / "01-preflight.json").read_text())["outputs"]
    assert pre["parent"]["spec"] == nxt["spec"] and pre["parent"]["leverage"] == "1.1782"
    mt = json.loads((receipts(root, "k-gm") / "06-match.json").read_text())["outputs"]
    assert mt["engine"] is True and mt["leverage_calibration"] == "1.1782"
    assert mt["calibration"]["leverage_requested"] == 1.1782
    flags = RC.load_spec(root / mt["cell_spec"])["nav"]["flags"]
    assert RC.load_spec(root / mt["cell_spec"])["nav"]["leverage"] == "1.1782"
    assert flags.count("--calibrate-gross") == 1 and flags.count("--calibrate-tolerance") == 1
