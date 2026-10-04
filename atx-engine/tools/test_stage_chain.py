"""stage_chain.py: resume after the last good receipt, refusal of a stale receipt, failed receipts, dry-run plans."""
from __future__ import annotations

import json
from pathlib import Path

import pytest

import stage_chain as SC


class Ctx:
    def __init__(self, root: Path):
        self.root, self.calls, self.fail = root, [], set()

    def file(self, name: str) -> Path:
        return self.root / name


def make(name: str):
    def run(ctx, done, log):
        ctx.calls.append(name)
        if name in ctx.fail:
            raise SC.StageError(f"{name} refused", 4)
        return {"value": name.upper(), "seen": sorted(done)}

    def inputs(ctx, done):
        p = ctx.file(f"{name}.in")
        return {"input": SC.sha256_file(p) if p.is_file() else None}

    def plan(ctx, done):
        return [f"tool {name} --after {','.join(sorted(done)) or '-'}"]
    return SC.Stage(name, run, inputs, plan)


def chain(tmp_path: Path) -> SC.Chain:
    return SC.Chain("test", [make("a"), make("b"), make("c")], tmp_path / "state", clock=lambda: "T")


def test_runs_in_order_and_resumes_after_the_last_good_receipt(tmp_path):
    ctx = Ctx(tmp_path)
    ch = chain(tmp_path)
    out = ch.run(ctx, until="b", log=lambda s: None)
    assert ctx.calls == ["a", "b"] and out["b"] == {"value": "B", "seen": ["a"]}
    assert [r["state"] for r in ch.state(ctx)] == ["done", "done", "pending"]
    ch.run(ctx, log=lambda s: None)
    assert ctx.calls == ["a", "b", "c"]                       # a and b skipped: their inputs are unchanged
    rec = json.loads(ch.receipt_path(2).read_text())
    assert rec["status"] == "ok" and rec["inputs"][SC.PREV] == SC.sha256_file(ch.receipt_path(1))
    ch.run(ctx, log=lambda s: None)
    assert ctx.calls == ["a", "b", "c"]                       # all done: nothing runs


def test_a_changed_input_is_refused_never_rerun(tmp_path):
    ctx = Ctx(tmp_path)
    ch = chain(tmp_path)
    ctx.file("b.in").write_text("v1")
    ch.run(ctx, log=lambda s: None)
    ctx.file("b.in").write_text("v2")
    assert [r["state"] for r in ch.state(ctx)] == ["done", "stale", "blocked"]
    with pytest.raises(SC.ChainError, match=r"STALE \[b\].*input") as e:
        ch.run(ctx, log=lambda s: None)
    assert e.value.code == SC.EXIT_STALE and ctx.calls == ["a", "b", "c"]
    ctx.file("b.in").write_text("v1")                         # restored: done again
    ch.run(ctx, log=lambda s: None)
    receipt = ch.receipt_path(0)                              # an edited earlier receipt makes the next one stale
    receipt.write_text(receipt.read_text().replace('"A"', '"Z"'))
    with pytest.raises(SC.ChainError, match=r"STALE \[b\].*previous_receipt_sha256"):
        ch.run(ctx, log=lambda s: None)


def test_a_failed_stage_leaves_a_failed_receipt_and_is_retried(tmp_path):
    ctx = Ctx(tmp_path)
    ch = chain(tmp_path)
    ctx.fail.add("b")
    with pytest.raises(SC.ChainError, match=r"HARD-STOP \[b\]: b refused") as e:
        ch.run(ctx, log=lambda s: None)
    assert e.value.code == 4 and not ch.receipt_path(1).exists()
    failed = json.loads((ch.receipt_dir() / "02-b.failed-1.json").read_text())
    assert failed["status"] == "failed" and failed["error"] == "b refused"
    with pytest.raises(SC.ChainError):
        ch.run(ctx, log=lambda s: None)
    assert (ch.receipt_dir() / "02-b.failed-2.json").is_file()   # never overwritten
    ctx.fail.clear()
    ch.run(ctx, log=lambda s: None)
    assert ctx.calls == ["a", "b", "b", "b", "c"]
    with pytest.raises(FileExistsError):                      # an ok receipt is written once
        SC.Chain.write(ch.receipt_path(2), {})


def test_any_exception_leaves_a_failed_receipt_with_code_4(tmp_path):
    """Review YINFRA #11: a KeyError / TypeError / JSON error after side effects is a failed receipt, not a traceback
    that leaves no record."""
    def boom(ctx, done, log):
        ctx.calls.append("x")
        return {"v": abs(None)}                                   # TypeError, as wave_stages' abs(None) was
    ctx = Ctx(tmp_path)
    ch = SC.Chain("test", [make("a"), SC.Stage("x", boom)], tmp_path / "state", clock=lambda: "T")
    with pytest.raises(SC.ChainError, match=r"HARD-STOP \[x\]: TypeError: bad operand") as e:
        ch.run(ctx, log=lambda s: None)
    failed = json.loads((ch.receipt_dir() / "02-x.failed-1.json").read_text())
    assert e.value.code == SC.EXIT_STOP and failed["code"] == 4 and failed["error"].startswith("TypeError: ")
    assert not ch.lock_path().exists()                            # released on the way out


def test_a_second_run_of_one_state_dir_is_refused_while_the_lock_is_held(tmp_path):
    ctx = Ctx(tmp_path)
    ch = chain(tmp_path)
    seen = []

    def nested(c, done, log):                                     # a second `run` while the first holds the dir
        with pytest.raises(SC.ChainError, match="is held") as e:
            chain(tmp_path).run(ctx, log=lambda s: None)
        seen.append(e.value.code)
        return {}
    ch.stages[1] = SC.Stage("b", nested)
    ch.run(ctx, log=lambda s: None)
    assert seen == [SC.EXIT_STOP] and ctx.calls == ["a", "c"] and not ch.lock_path().exists()
    ch.lock_path().write_text('{"pid": 1}\n')                     # a crashed run's leftover: refused until removed
    with pytest.raises(SC.ChainError, match='"pid": 1'):
        chain(tmp_path).run(ctx, log=lambda s: None)


def test_plan_runs_nothing_and_marks_done_stages(tmp_path):
    ctx = Ctx(tmp_path)
    ch = chain(tmp_path)
    assert ch.plan(ctx) == ["# stage 01 a: pending", "tool a --after -", "# stage 02 b: pending", "tool b --after -",
                            "# stage 03 c: pending", "tool c --after -"]
    assert ctx.calls == [] and not ch.receipt_dir().exists()
    ch.run(ctx, until="a", log=lambda s: None)
    lines = ch.plan(ctx)
    assert lines[0].startswith("# stage 01 a: done") and lines[2] == "tool b --after a"


def test_plan_marks_a_stale_receipt_not_done(tmp_path):
    """Review YINFRA #16: plan() uses state()'s input diff; a receipt whose input moved is not "done"."""
    ctx = Ctx(tmp_path)
    ch = chain(tmp_path)
    ctx.file("a.in").write_text("v1")
    ch.run(ctx, until="b", log=lambda s: None)
    ctx.file("a.in").write_text("v2")
    lines = ch.plan(ctx)
    assert lines[0].startswith("# stage 01 a: STALE (inputs changed: input)") and "run refuses it" in lines[0]
    assert lines[1] == "# stage 02 b: blocked (stage a is stale)"
    assert lines[3] == "# stage 03 c: blocked (stage a is stale)"
    assert ctx.calls == ["a", "b"]


def test_stage_names_and_unknown_until(tmp_path):
    with pytest.raises(ValueError):
        SC.Chain("x", [make("a"), make("a")], tmp_path)
    with pytest.raises(SC.ChainError) as e:
        chain(tmp_path).run(Ctx(tmp_path), until="zz", log=lambda s: None)
    assert e.value.code == 2
    with pytest.raises(ValueError, match="digest must be one of file, content"):
        SC.Chain("x", [make("a")], tmp_path, digest="mtime")


def test_content_digest_chains_without_time_keys(tmp_path):
    """P9 OR section 3: digest="content" chains each receipt's content digest (canonical JSON without started_utc and
    seconds) as previous_receipt_content_sha256, so two runs at different times chain the same digests; the default
    "file" rule keeps previous_receipt_sha256 (the file's SHA-256, time included); a state dir is read under one
    rule (a receipt recorded under the other is stale)."""
    digests = {}
    for clock, rule in (("2020-01-02T00:00:00", "content"), ("2021-06-30T12:00:00", "content"),
                        ("2020-01-02T00:00:00", "file"), ("2021-06-30T12:00:00", "file")):
        ctx = Ctx(tmp_path / f"{rule}-{clock[:4]}")
        ctx.root.mkdir()
        ch = SC.Chain("test", [make("a"), make("b")], ctx.root / "state", clock=lambda c=clock: c, digest=rule)
        ch.run(ctx, log=lambda s: None)
        rec = json.loads(ch.receipt_path(1).read_text())
        key = SC.PREV_CONTENT if rule == "content" else SC.PREV
        assert set(rec["inputs"]) == {"input", key}
        assert rec["inputs"][key] == ch.receipt_digest(ch.receipt_path(0))
        digests.setdefault(rule, set()).add(rec["inputs"][key])
    assert len(digests["content"]) == 1 and len(digests["file"]) == 2          # time-free vs time-dependent
    p = tmp_path / "r.json"
    p.write_text(json.dumps({"stage": "a", "outputs": {"x": 1}, "started_utc": "T1", "seconds": 1.5}))
    first = SC.content_sha256(p)
    p.write_text(json.dumps({"seconds": 9.0, "started_utc": "T2", "outputs": {"x": 1}, "stage": "a"}, indent=2))
    assert SC.content_sha256(p) == first                                      # time keys, order, layout aside
    p.write_text(json.dumps({"stage": "a", "outputs": {"x": 2}, "started_utc": "T1", "seconds": 1.5}))
    assert SC.content_sha256(p) != first                                      # content is digested
    ctx = Ctx(tmp_path / "content-2020")                                      # one rule per state dir
    ch = SC.Chain("test", [make("a"), make("b")], ctx.root / "state", clock=lambda: "T")
    with pytest.raises(SC.ChainError, match=r"STALE \[b\].*previous_receipt_content_sha256, previous_receipt_sha256") \
            as e:
        ch.run(ctx, log=lambda s: None)
    assert e.value.code == SC.EXIT_STALE
