"""A resumable chain of stages with hash-pinned receipts (generic machinery; platform v8 lane YINFRA).

A chain is an ordered list of stages. ``run`` walks them in order; each stage is in one state:

  done     an ok receipt exists and the inputs it recorded equal the inputs computed now: skipped
  stale    an ok receipt exists but an input changed since it was written: refused (ChainError, code 3); a stage is
           never re-run silently over a receipt
  pending  no ok receipt: the stage runs. Its inputs always include the SHA-256 of the previous stage's ok receipt
           file, so a stage never runs before the stage it follows, and an edited earlier receipt makes every later
           receipt stale
  blocked  a later stage while an earlier one is pending, stale or failed (``state`` only)

A stage is ``Stage(name, run, inputs=None, plan=None)``:
  inputs(ctx, done) -> {name: JSON value}   what the stage consumes (digests of files, pins, rule names); recorded
  run(ctx, done, log) -> {name: JSON value} its outputs, recorded in the receipt and handed to later stages as
                                            ``done[name]``
  plan(ctx, done) -> [line, ...]            the exact command lines the stage would run (dry run; nothing runs)
``done`` maps each earlier stage name to its recorded outputs (an empty dict for a stage not run yet in a dry run).

Receipts: ``<state dir>/receipts/<NN>-<name>.json`` = {schema, chain, stage, index, status "ok", inputs, outputs,
started_utc, seconds}, written once (never overwritten). A stage that raises (StageError with its code, any other
exception with code 4 and its type named) leaves ``<NN>-<name>.failed-<k>.json`` (k the first free number; kept for
the record, never read for resume) and stops the chain; the next ``run`` retries it. ``run`` holds
``<state dir>/chain.lock`` (created O_EXCL, removed when it ends): a second run of the same state dir is refused.

Digest (``Chain(..., digest=)``): "file" (the default, as before) chains the previous receipt's file SHA-256 as the
input ``previous_receipt_sha256``; "content" (P9 OR section 3) chains ``content_sha256``, the SHA-256 of its canonical
JSON without the time keys (TIME_KEYS: started_utc, seconds), as ``previous_receipt_content_sha256``, so a chain re-run
on the same inputs and outputs has the same digests whatever its timing. A chain keeps one rule: a receipt recorded
under the other rule reads as stale.
Standard library only.
"""
from __future__ import annotations

from dataclasses import dataclass
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import time
from typing import Callable

SCHEMA = "atx.stage-receipt/v1"
PREV = "previous_receipt_sha256"          # the input every stage after the first carries
PREV_CONTENT = "previous_receipt_content_sha256"   # its name under digest="content" (P9 OR section 3)
TIME_KEYS = ("started_utc", "seconds")    # a receipt's time keys: out of its content digest
DIGESTS = ("file", "content")
LOCK = "chain.lock"                       # <state dir>/chain.lock while a run holds the state dir
EXIT_STALE, EXIT_STOP = 3, 4


class ChainError(Exception):
    """The chain stopped: a stale receipt (code 3) or a failed stage (its StageError code)."""

    def __init__(self, message: str, code: int = EXIT_STOP):
        super().__init__(message)
        self.code = code


class StageError(Exception):
    """A stage's own refusal or failure (the chain records a failed receipt and stops)."""

    def __init__(self, message: str, code: int = EXIT_STOP):
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class Stage:
    name: str
    run: Callable
    inputs: Callable | None = None
    plan: Callable | None = None


def canonical(value) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")


def content_sha256(path: Path) -> str:
    """SHA-256 of a receipt's content keys: its canonical JSON without TIME_KEYS (P9 OR section 3)."""
    doc = json.loads(Path(path).read_text(encoding="utf-8"))
    body = {k: v for k, v in doc.items() if k not in TIME_KEYS} if isinstance(doc, dict) else doc
    return hashlib.sha256(canonical(body).encode("utf-8")).hexdigest()


class Chain:
    def __init__(self, name: str, stages: list[Stage], state_dir: Path, *, clock: Callable[[], str] = utc_now,
                 digest: str = "file"):
        names = [s.name for s in stages]
        if not stages or len(set(names)) != len(names) or not all(n and n.replace("-", "").isalnum() for n in names):
            raise ValueError(f"chain {name}: stages need distinct names of letters, digits and '-' ({names})")
        if digest not in DIGESTS:
            raise ValueError(f"chain {name}: digest must be one of {', '.join(DIGESTS)}")
        self.name, self.stages, self.state_dir, self.clock = name, list(stages), Path(state_dir), clock
        self.digest = digest

    def receipt_digest(self, path: Path) -> str:
        """A receipt file's digest under the chain's rule: its file SHA-256, or content_sha256 (digest="content")."""
        return content_sha256(path) if self.digest == "content" else sha256_file(path)

    # -------------------------------------------------------------- receipts
    def receipt_dir(self) -> Path:
        return self.state_dir / "receipts"

    def receipt_path(self, index: int) -> Path:
        return self.receipt_dir() / f"{index + 1:02d}-{self.stages[index].name}.json"

    def read(self, index: int) -> dict | None:
        p = self.receipt_path(index)
        if not p.is_file():
            return None
        doc = json.loads(p.read_text(encoding="utf-8"))
        if not isinstance(doc, dict) or doc.get("schema") != SCHEMA or doc.get("stage") != self.stages[index].name \
                or doc.get("status") != "ok":
            raise ChainError(f"receipt {p} is not an ok {SCHEMA} receipt of stage {self.stages[index].name}",
                             EXIT_STALE)
        return doc

    def failed_path(self, index: int) -> Path:
        base = self.receipt_path(index)
        k = 1
        while base.with_name(f"{base.stem}.failed-{k}.json").exists():
            k += 1
        return base.with_name(f"{base.stem}.failed-{k}.json")

    def stage_inputs(self, index: int, ctx, done: dict) -> dict:
        st = self.stages[index]
        own = dict(st.inputs(ctx, done)) if st.inputs else {}
        if PREV in own or PREV_CONTENT in own:
            raise ValueError(f"stage {st.name}: input names {PREV}, {PREV_CONTENT} are the chain's own")
        if index > 0:
            prev = self.receipt_path(index - 1)
            own[PREV_CONTENT if self.digest == "content" else PREV] = \
                self.receipt_digest(prev) if prev.is_file() else None
        return json.loads(canonical(own))

    @staticmethod
    def changed(recorded: dict, now: dict) -> list[str]:
        return sorted(k for k in set(recorded) | set(now) if canonical(recorded.get(k)) != canonical(now.get(k)))

    # -------------------------------------------------------------- state / plan / run
    def state(self, ctx) -> list[dict]:
        """[{stage, state, receipt, why}] without running anything (input changes are detected, never raised)."""
        rows, done, blocked = [], {}, False
        for i, st in enumerate(self.stages):
            if blocked:
                rows.append({"stage": st.name, "state": "blocked", "receipt": None, "why": "an earlier stage is open"})
                continue
            rec = self.read(i)
            if rec is None:
                rows.append({"stage": st.name, "state": "pending", "receipt": None, "why": ""})
                blocked = True
                continue
            try:
                diff = self.changed(rec["inputs"], self.stage_inputs(i, ctx, done))
            except (StageError, OSError, ValueError, KeyError) as exc:
                diff = [f"<inputs unavailable: {exc}>"]
            if diff:
                rows.append({"stage": st.name, "state": "stale", "receipt": str(self.receipt_path(i)),
                             "why": f"inputs changed: {', '.join(diff)}"})
                blocked = True
                continue
            done[st.name] = rec["outputs"]
            rows.append({"stage": st.name, "state": "done", "receipt": str(self.receipt_path(i)), "why": ""})
        return rows

    def plan(self, ctx) -> list[str]:
        """Every stage's planned command lines (dry run), on ``state``'s input diff: a done stage (receipt and inputs
        unchanged) is marked done; a stale one is marked STALE with its changed inputs (``run`` refuses it) and the
        stages after it blocked; a pending stage and those after it print their plan()."""
        out: list[str] = []
        done: dict = {}
        stale = None
        for i, (st, row) in enumerate(zip(self.stages, self.state(ctx))):
            head = f"# stage {i + 1:02d} {st.name}"
            if row["state"] == "done":
                done[st.name] = self.read(i)["outputs"]
                out.append(f"{head}: done ({self.receipt_path(i)})")
                continue
            if row["state"] == "stale":
                stale = st.name
                out.append(f"{head}: STALE ({row['why']}): run refuses it (restore the inputs, or a new state dir)")
                continue
            out.append(f"{head}: blocked (stage {stale} is stale)" if stale else f"{head}: pending")
            out += list(st.plan(ctx, done)) if st.plan else []
        return out

    def lock_path(self) -> Path:
        return self.state_dir / LOCK

    def acquire(self) -> None:
        """The state dir's run lock (O_CREAT | O_EXCL): one ``run`` per state dir at a time."""
        self.state_dir.mkdir(parents=True, exist_ok=True)
        try:
            fd = os.open(self.lock_path(), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except FileExistsError:
            try:
                held = self.lock_path().read_text(encoding="utf-8").strip()
            except OSError:
                held = "unreadable"
            raise ChainError(f"chain {self.name}: {self.lock_path()} is held ({held}): another run of this state dir "
                             "is in progress; a crashed run leaves it behind (check that no run is alive, then remove "
                             "it)", EXIT_STOP) from None
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(json.dumps({"pid": os.getpid(), "started_utc": self.clock()}) + "\n")

    def run(self, ctx, *, until: str | None = None, log=print) -> dict:
        """Run the pending stages in order (see the module doc) under the state dir's lock; returns {stage: outputs}
        of every ok stage."""
        if until is not None and until not in [s.name for s in self.stages]:
            raise ChainError(f"chain {self.name}: no stage {until!r}", 2)
        self.acquire()
        try:
            return self._run(ctx, until, log)
        finally:
            self.lock_path().unlink(missing_ok=True)

    def _run(self, ctx, until: str | None, log) -> dict:
        done: dict = {}
        for i, st in enumerate(self.stages):
            rec = self.read(i)
            now = self.stage_inputs(i, ctx, done)
            if rec is not None:
                diff = self.changed(rec["inputs"], now)
                if diff:
                    raise ChainError(f"STALE [{st.name}]: its inputs changed since {self.receipt_path(i)} was written "
                                     f"({', '.join(diff)}); the stage is never re-run over its receipt: restore the "
                                     "inputs, or start a new state dir", EXIT_STALE)
                done[st.name] = rec["outputs"]
                log(f"== {st.name}: done ({self.receipt_path(i).name})")
            else:
                log(f"== {st.name}")
                started, t0 = self.clock(), time.monotonic()
                try:
                    outputs = st.run(ctx, done, log)
                except Exception as exc:                  # noqa: BLE001  any failure leaves its receipt and stops
                    code = exc.code if isinstance(exc, StageError) else EXIT_STOP
                    error = str(exc) if isinstance(exc, StageError) else f"{type(exc).__name__}: {exc}"
                    self.write(self.failed_path(i), dict(self.body(i, now, {}, started, t0), status="failed",
                                                         error=error, code=code))
                    raise ChainError(f"HARD-STOP [{st.name}]: {error}", code) from exc
                outputs = json.loads(canonical(outputs or {}))
                self.write(self.receipt_path(i), self.body(i, now, outputs, started, t0))
                done[st.name] = outputs
                log(f"   receipt {self.receipt_path(i)}")
            if until == st.name:
                log(f"== stopped after {st.name}")
                break
        return done

    def body(self, index: int, inputs: dict, outputs: dict, started: str, t0: float) -> dict:
        return {"schema": SCHEMA, "chain": self.name, "stage": self.stages[index].name, "index": index + 1,
                "status": "ok", "inputs": inputs, "outputs": outputs, "started_utc": started,
                "seconds": round(time.monotonic() - t0, 3)}

    @staticmethod
    def write(path: Path, doc: dict) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("x", encoding="utf-8", newline="\n") as f:      # never overwritten
            f.write(json.dumps(doc, indent=2, sort_keys=True, allow_nan=False) + "\n")
