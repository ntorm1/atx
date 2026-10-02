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


def test_plan_runs_nothing_and_marks_done_stages(tmp_path):
    ctx = Ctx(tmp_path)
    ch = chain(tmp_path)
    assert ch.plan(ctx) == ["# stage 01 a: pending", "tool a --after -", "# stage 02 b: pending", "tool b --after -",
                            "# stage 03 c: pending", "tool c --after -"]
    assert ctx.calls == [] and not ch.receipt_dir().exists()
    ch.run(ctx, until="a", log=lambda s: None)
    lines = ch.plan(ctx)
    assert lines[0].startswith("# stage 01 a: done") and lines[2] == "tool b --after a"


def test_stage_names_and_unknown_until(tmp_path):
    with pytest.raises(ValueError):
        SC.Chain("x", [make("a"), make("a")], tmp_path)
    with pytest.raises(SC.ChainError) as e:
        chain(tmp_path).run(Ctx(tmp_path), until="zz", log=lambda s: None)
    assert e.value.code == 2
