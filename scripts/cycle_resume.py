"""Resume check of a done NAV output (platform v8 review W1-C, C-13; the ref phase's too, review F-6), used by
research_cycle.run_cycle.

A NAV step is done when its output has summary.json; resume then scores it (summ). Before this check nothing tied that
output to the spec being run: a spec re-locked after its library or weights changed, output names unchanged, bound
the new spec_sha256 in cycle_verdict.json to the old cell's numbers. Now:

  record  when the cycle runs a NAV step, it writes <run dir>/cycle_binding.json = {schema, output, spec_sha256 (the
          cell's spec digest, the verdict's spec_sha256: research_spec.spec_digest, review F-9: a template's covers the
          template and every parent up its chain), spec_rule (how spec_sha256 was computed: SPEC_RULE), argv_sha256
          (SHA-256 of the NAV command after its executable: every pin the step passes, combined signal, role, fields,
          rule and flags)}
  check   a done NAV is scored only when its recorded spec digest equals the current spec's (a binding without
          spec_rule, written before review F-9, recorded the spec file's own SHA-256 and is compared with that) AND its
          argv digest equals the NAV command this spec would run now (review F-9: a matching spec digest never skips
          the argv check), from the binding or else from its bounded-runner receipt (outcome completed, exit 0,
          ``command`` = the executable and its argv). A mismatch, or an output with neither, is refused with the
          digests in the message.
"""
from __future__ import annotations

import hashlib
import json

import research_spec
import research_tree

BINDING = "cycle_binding.json"
SCHEMA = "atx.cycle-nav-binding/v1"
SPEC_RULE = "spec-digest-v1"           # review F-9: spec_sha256 = research_spec.spec_digest (a template's chain)


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
    """The cell's spec digest (review F-9: research_spec.spec_digest; None for a cycle without a spec file)."""
    if not cycle.spec_path:
        return None
    return research_spec.spec_digest(cycle.spec_path, research_tree.REPO)


def file_digest(cycle) -> str | None:
    """The spec file's own SHA-256: what a binding written before review F-9 recorded as spec_sha256."""
    return research_spec.file_sha256(cycle.spec_path) if cycle.spec_path else None


def write_binding(cycle, st) -> dict:
    """<run dir>/cycle_binding.json of a NAV step this invocation ran (review C-13): the cell (nav) binds its spec
    digest and argv; the reference construction (ref, review F-6) its argv only, since its identity compare depends on
    nothing else and a spec edit elsewhere must not force it to run again."""
    doc = {"schema": SCHEMA, "output": st.output, "spec_sha256": spec_digest(cycle) if st.phase == "nav" else None}
    if doc["spec_sha256"] is not None:
        doc["spec_rule"] = SPEC_RULE
    doc["argv_sha256"] = argv_digest(step_args(st))
    cycle.res.path(f"{st.run_dir}/{BINDING}").write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    return doc


def check_binding(cycle, st) -> str:
    """Raise ResumeError unless the done NAV ``st`` was made from the current spec and by the NAV command it would run
    now (see the module doc); returns how it was matched (for the log)."""
    rec = cycle.res.read_json(f"{st.run_dir}/{BINDING}")
    rec = rec if isinstance(rec, dict) else {}
    matched = []
    if rec.get("spec_sha256"):
        spec_now = spec_digest(cycle) if rec.get("spec_rule") == SPEC_RULE else file_digest(cycle)
        if spec_now:
            if rec["spec_sha256"] != spec_now:
                raise ResumeError(f"NAV {st.output} was made under spec sha256 {rec['spec_sha256']}; the current spec "
                                  f"is sha256 {spec_now}: refusing to score it (run the NAV again under a fresh "
                                  "--suffix)")
            matched.append(f"spec sha256 {spec_now}")
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
    return ", ".join(matched + [f"argv sha256 {now} ({source})"])
