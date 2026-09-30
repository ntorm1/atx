"""Resume check of a done NAV output (platform v8 review W1-C, C-13), used by research_cycle.run_cycle.

A NAV step is done when its output has summary.json; resume then scores it (summ). Before this check nothing tied that
output to the spec being run: a spec re-locked after its library or weights changed, output names unchanged, bound
the new spec_sha256 in cycle_verdict.json to the old cell's numbers. Now:

  record  when the cycle runs a NAV step, it writes <run dir>/cycle_binding.json = {schema, output, spec_sha256 (the
          spec file's SHA-256, the verdict's spec_sha256), argv_sha256 (SHA-256 of the NAV command after its
          executable: every pin the step passes, combined signal, role, fields, rule and flags)}
  check   a done NAV is scored only when its recorded spec digest equals the current spec's; an output without a
          spec digest (made before this check, or by a cycle without a spec file) is checked on its argv digest, from
          the binding or else from its bounded-runner receipt (outcome completed, exit 0, ``command`` = the executable
          and its argv). A mismatch, or an output with neither, is refused with the digests in the message.
"""
from __future__ import annotations

import hashlib
import json

BINDING = "cycle_binding.json"
SCHEMA = "atx.cycle-nav-binding/v1"


class ResumeError(ValueError):
    """A done output that cannot be scored under the current spec (research_cycle.py: a pin stop, exit 3)."""


def argv_digest(args: list[str]) -> str:
    """SHA-256 of a command's arguments after its executable (compact JSON list)."""
    return hashlib.sha256(json.dumps([str(a) for a in args], separators=(",", ":")).encode()).hexdigest()


def step_args(st) -> list[str]:
    """A bounded step's command after its executable: what the runner records as ``command[1:]``."""
    k = st.argv.index("--")
    return st.argv[k + 2:]


def spec_digest(cycle) -> str | None:
    if not cycle.spec_path:
        return None
    h = hashlib.sha256()
    with open(cycle.spec_path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_binding(cycle, st) -> dict:
    """<run dir>/cycle_binding.json of a NAV step this invocation ran (review C-13)."""
    doc = {"schema": SCHEMA, "output": st.output, "spec_sha256": spec_digest(cycle),
           "argv_sha256": argv_digest(step_args(st))}
    cycle.res.path(f"{st.run_dir}/{BINDING}").write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    return doc


def check_binding(cycle, st) -> str:
    """Raise ResumeError unless the done NAV ``st`` was made from the current spec (see the module doc); returns how it
    was matched (for the log)."""
    rec = cycle.res.read_json(f"{st.run_dir}/{BINDING}")
    rec = rec if isinstance(rec, dict) else {}
    spec_now = spec_digest(cycle)
    if rec.get("spec_sha256") and spec_now:
        if rec["spec_sha256"] != spec_now:
            raise ResumeError(f"NAV {st.output} was made under spec sha256 {rec['spec_sha256']}; the current spec is "
                              f"sha256 {spec_now}: refusing to score it (run the NAV again under a fresh --suffix)")
        return f"spec sha256 {spec_now}"
    now = argv_digest(step_args(st))
    made, source = rec.get("argv_sha256"), f"{st.run_dir}/{BINDING}"
    if made is None:
        r = cycle.receipt(st.run_dir)
        source = f"{st.run_dir}/receipt.json"
        if not (isinstance(r, dict) and r.get("outcome") == "completed" and r.get("exit_code") == 0 and
                isinstance(r.get("command"), list) and r["command"]):
            raise ResumeError(f"NAV {st.output} has no cycle binding and no completed receipt with its command in "
                              f"{st.run_dir}: cannot tell whether it was made from this spec (NAV argv sha256 {now}); "
                              "refusing to score it")
        made = argv_digest(r["command"][1:])
    if made != now:
        raise ResumeError(f"NAV {st.output} was made by a command with argv sha256 {made} ({source}); this spec's NAV "
                          f"command is argv sha256 {now}: refusing to score it (run the NAV again under a fresh "
                          "--suffix)")
    return f"argv sha256 {now} ({source})"
