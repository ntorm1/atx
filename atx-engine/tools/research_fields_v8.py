"""Platform v8 research fields (lane F3): ``grp_ff12f49`` (spec F-A of the library v8 draft, section 8).

An opt-in field module of ``prepare_research_fields.py``, like ``research_fields_price.py``: the builder's
``FIELD_MODULES`` hook binds it (``bind(globals())``), ``check`` runs before any output and ``compute`` after the
built-in groups and the modules registered before it. Nothing else in the builder changes, so every other field's
bytes and manifest entry are identical whether or not these fields are requested.

Registration: ``bind`` adds ``FIELDS`` to the builder's ``ALL_FIELDS`` itself (after every field registered before
it), so the builder hook is the import and ``FIELD_MODULES.append(...)`` only. A module-level
``ALL_FIELDS.update(...)`` statement in the builder would join the host closure of every module that reads
``spec_definition`` through its handle (the SEC module), change the SEC producer fingerprint and so recompute every
SEC field once under ``--reuse``.

``--reuse`` (v8 C-3 contract): ``PRODUCERS``, ``HOST_HANDLES``, ``producer_group``, ``field_spec``, ``reuse_inputs``
and ``entry_inputs``; every computed entry records ``producer`` (this module's code identity).

Fields:
* ``grp_ff12f49``: FF12 with the Money industry split by FF49 (Banks, Insur, RlEst, Fin): a cellwise function of this
  run's ``grp_ff12`` and ``grp_ff49`` payloads (same SIC row, clock and 550-day staleness). It lets a DSL member rank
  financial firms within their FF49 industry and every other firm within its FF12 industry (R2-8, S-12), which no DSL
  expression can do (group fields cannot be compared or selected).
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import sys

import numpy as np

from research_fields_sec import _Host  # the SEC module's late-bound host view (same directory)

GROUP = "v8"
OPTIONS = ()
HOST_HANDLES = ("h",)
FF12_MONEY = 11.0                       # French Siccodes12 "Money" (prepare_research_fields.FF12_NUMBERS)
FF49_MONEY = (45.0, 46.0, 47.0, 48.0)   # French Siccodes49 Banks, Insur, RlEst, Fin
MONEY_OFFSET = 100.0
MONEY_CODES = {"145": "Banks", "146": "Insur", "147": "RlEst", "148": "Fin", "11": "Money, no FF49 industry"}

FF12F49_RULE = ("ff12-money-ff49-v1, cellwise on row t: ff12 = grp_ff12[t], ff49 = grp_ff49[t]; 100 + ff49 when "
                "ff12 == 11 (Money) and ff49 in {45, 46, 47, 48} (145 Banks, 146 Insur, 147 RlEst, 148 Fin); "
                "otherwise ff12 (a Money SIC that Siccodes49 does not list -> 11; every non-Money cell -> ff12 bit for "
                "bit, 1-10 and 12; ff12 NaN -> NaN)")
FF12F49_CLOCK = ("the clock of this run's grp_ff12 and grp_ff49 (fund-events-lagged-v1 on the SIC table: the linked "
                 "CIK's latest visible valid SIC row, --fund-lag-sessions; both from the same row): row t reads their "
                 "row t only")

FIELDS = {
    "grp_ff12f49": {
        "group": "v8_ff12f49", "point_in_time": True, "lagged": False,
        "units": ("categorical code: Fama-French 12-industry number with FF12 Money (11) split by FF49: 145 Banks, "
                  "146 Insur, 147 RlEst, 148 Fin; 11 for a Money SIC listed under no FF49 industry; 1-10 and 12 "
                  "otherwise; stored as f64"),
        "clock": FF12F49_CLOCK,
        "staleness": ("inherits grp_ff12 (no visible SIC row or a row older than 550 days -> NaN): NaN wherever "
                      "grp_ff12 is NaN; no fill"),
        "definition": FF12F49_RULE,
        "source_columns": ["grp_ff12", "grp_ff49"],
        "caveats": [
            "REITs (SIC 6798) are FF49 48 Fin (Trading) in French's Siccodes49, not 47 RlEst",
            "a Money SIC that Siccodes49 does not list keeps the FF12 Money code 11 (one group of such firms)",
            "inherits every grp_ff12 / grp_ff49 caveat: SIC as of each filing (FSDS SUB), the pinned French mapping "
            "tables, identity from the pinned identity bridge, values on primary lines only"],
        "formula_id": "ff12-money-ff49-v1",
        "min_history": "as grp_ff12 (one visible valid SIC row)",
        "requires": ["grp_ff12", "grp_ff49"],
    },
}
# The producing code of each field group (contract of task C-3: {group: (entry names,)}); the module-level
# definitions they reach are part of each producer. compute() orchestrates and is never a producer.
PRODUCERS = {"v8_ff12f49": ("ff12f49_rows",)}


# ---------------------------------------------------------------------------------------------------------------
# Inputs: this run's published payloads
# ---------------------------------------------------------------------------------------------------------------

class PublishedRows:
    """This run's published payloads of ``names`` (fields a module field requires), read row by row in session order
    and hashed as read. Each must be a complete date-major f64 file of the role's shape."""

    def __init__(self, output: Path, role, names, what: str):
        self.what, self.size, self.n_dates = what, role.n * 8, role.n_dates
        self.paths = {x: Path(output) / f"{x}.f64" for x in names}
        for x, p in self.paths.items():
            if not p.is_file() or p.stat().st_size != role.n_dates * self.size:
                raise ValueError(f"{what}: this run's {x}.f64 is missing or has the wrong size")
        self.hashes = {x: hashlib.sha256() for x in names}
        self.files = {}
        try:
            for x, p in self.paths.items():
                self.files[x] = p.open("rb")
        except BaseException:
            self.close()
            raise

    def row(self) -> dict:
        out = {}
        for x, f in self.files.items():
            blob = f.read(self.size)
            if len(blob) != self.size:
                raise ValueError(f"{self.what}: this run's {x}.f64 is truncated")
            self.hashes[x].update(blob)
            out[x] = np.frombuffer(blob, dtype="<f8")
        return out

    def sources(self) -> list:
        """One source per payload read (after every row): path, bytes, SHA-256 of the bytes read, field."""
        for x, f in self.files.items():
            if f.read(1):
                raise ValueError(f"{self.what}: this run's {x}.f64 is longer than the role shape")
        return [{"path": str(p.resolve()), "bytes": self.n_dates * self.size, "sha256": self.hashes[x].hexdigest(),
                 "field": x} for x, p in self.paths.items()]

    def close(self):
        for f in self.files.values():
            f.close()


# ---------------------------------------------------------------------------------------------------------------
# Producers: each returns {name: (writer, sources, extras)}
# ---------------------------------------------------------------------------------------------------------------

def ff12f49_code(ff12: np.ndarray, ff49: np.ndarray) -> np.ndarray:
    """``FF12F49_RULE`` on arrays of FF12 and FF49 codes (NaN = unknown label)."""
    money = (ff12 == FF12_MONEY) & np.isin(ff49, FF49_MONEY)
    return np.where(money, MONEY_OFFSET + ff49, ff12)


def ff12f49_rows(h, role, output: Path, budget) -> dict:
    """``grp_ff12f49`` (its field definition) from this run's published grp_ff12.f64 and grp_ff49.f64."""
    rows = PublishedRows(output, role, ("grp_ff12", "grp_ff49"), "grp_ff12f49")
    counts = dict.fromkeys(MONEY_CODES, 0)
    w = None
    try:
        try:
            w = h.FieldWriter(output, "grp_ff12f49", role)
            for t in range(role.n_dates):
                got = rows.row()
                code = ff12f49_code(got["grp_ff12"], got["grp_ff49"])
                w.write(code)
                member = role.member[t] != 0
                for key in counts:
                    counts[key] += int(np.count_nonzero(member & (code == float(key))))
                if t % 256 == 0:
                    budget.check("v8-ff12f49-write")
            sources = rows.sources()
        finally:
            rows.close()
    except BaseException:
        if w is not None:
            w.f.close()   # refused: the partial file stays unpublished (no manifest), handle released
        raise
    w.close()
    return {"grp_ff12f49": (w, sources, {"money_member_cells": counts})}


# ---------------------------------------------------------------------------------------------------------------
# The module object the builder binds, and the --reuse interface (v8 C-3)
# ---------------------------------------------------------------------------------------------------------------

def bind(host_namespace: dict) -> "V8FieldModule":
    """Register FIELDS in the builder's registry (after every field registered so far) and return the module object."""
    registry = host_namespace["ALL_FIELDS"]
    clash = [x for x in FIELDS if x in registry and registry[x] is not FIELDS[x]]
    if clash:
        raise ValueError(f"research_fields_v8: {', '.join(clash)} already registered by another producer")
    registry.update(FIELDS)
    return V8FieldModule(host_namespace)


def producer_group(name: str) -> str:
    return FIELDS[name]["group"]


def field_spec(name: str) -> dict:
    return FIELDS[name]


def reuse_inputs(name: str, options: dict) -> dict:
    """This run's input pins of a field beyond the fields it requires (none for grp_ff12f49: the reuse rule's
    'every field it requires is reused' is its input check)."""
    return {}


def entry_inputs(entry: dict) -> dict:
    """The same pins as a manifest entry records them."""
    return {}


class V8FieldModule:
    GROUP, FIELDS, OPTIONS = GROUP, FIELDS, OPTIONS

    def __init__(self, host_namespace: dict):
        self.h = _Host(host_namespace)

    @staticmethod
    def add_arguments(parser):
        """No options: grp_ff12f49 reads this run's grp_ff12 / grp_ff49 (named in --fields, the builder checks it)."""

    @staticmethod
    def check(selected, options: dict):
        """Nothing beyond the builder's own 'requires' check before any output."""

    def compute(self, names, role, output: Path, budget, options: dict, outcome: dict, source_checks: dict,
                field_extras: dict):
        if not names:
            return
        h = self.h
        results = {}
        if "grp_ff12f49" in names:
            results.update(ff12f49_rows(h, role, output, budget))
        producer = {"module": Path(__file__).name, **h.module_code_identity(sys.modules[__name__])}
        for x in names:
            w, sources, extra = results[x]
            spec = FIELDS[x]
            field_extras[x] = {"producer": producer, "formula_id": spec["formula_id"],
                               "formula_sha256": h.formula_id(x, h.spec_definition(x, 0)),
                               "min_history": spec["min_history"], **extra}
            outcome[x] = (w, sources, w.coverage())
        budget.report("v8-complete", fields=len(names))
