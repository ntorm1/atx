"""Sealed reader of the atx-db gold alpha-panel pipeline stages (lane XDATA task GOLD, Ruling PM7-19). Draft, off by
default.

``prepare_research_fields.py`` does not register this module; ``prepare_research_fields_xdata.py`` does (``bind``
appends ``FIELDS`` to the builder's registry, the entry appends the module object to ``FIELD_MODULES``), and a gold
field also needs ``--gold-panel-root`` and its stage manifest pin: absent options, nothing is read.

Sources (atx-db, read only): the alpha-panel build root's ``gold/`` stage (schema ``atx.alpha-panel.gold/v1``:
``year=YYYY/gold.parquet`` keyed (session_date, security_id) with ``cik``, ``grp_ff49``, ``grp_ff12``, ``g_*`` scores,
``c_*`` composites and ``ctl_*`` raw controls) and its ``characteristics/`` stage (``atx.alpha-panel.characteristics/v2``:
``year=YYYY/characteristics.parquet``, the raw prior-signed registry features). Each is bound by the SHA-256 of its
``manifest.json`` (``--gold-sha256`` / ``--gold-characteristics-sha256``), must say ``status: complete``, and every
file opened must match the manifest's bytes and SHA-256.

What is never read (refused by name before any output, ``refused_reason``):
* a stage other than ``gold`` and ``characteristics`` (labels, labels_goldready, labels_holdout, validation, ic, ...);
* a column whose name has a label, forward, future, IC, OOS, holdout, target, outcome or lead token;
* the gold ``g_*`` scores and ``c_*`` composites (``GoldSelectionColumn``): their presence, horizon and variant were
  chosen by an IC screen on 2019-2021 forward returns (atx-db ``gold.select``), inside this program's TRAIN window, so
  binding one imports a mined selection that no trial count records. ``ALLOW_ADMITTED_SCORES`` stays False until the PM
  rules otherwise.
Only the key columns and the declared field columns are read (column projection), so no other column of a stage
file is ever loaded.

Seal (reader side, research_window): ``compute`` refuses unless the builder's ``SEAL`` is ``research_window.SEAL``; a
year partition that begins on or after the seal is never opened (``rw.partition_is_sealed``); the date filter
``session_date < SEAL`` (and the role's own range) is pushed down into the parquet read, so row groups wholly on or
after the seal are skipped by their statistics and no such row reaches this code; a row on or after the seal in the
read result is refused (``SealError``), never dropped silently.

Stamping: atx-db stamps a characteristic or gold value at (d, line) with information known by d's 22:00 UTC mark
(same-session closes; implied volatility of d-1; fundamentals with clock < 22:00 UTC of d-1; the panel's as-of rules
for short interest, short volume (trade date d-1), FTD, Reg SHO and 13F; same-session member_equity cross sections).
This reader adds one session: field row t holds the panel value of the session before t (``LAG_SESSIONS`` = 1, the
house t-1 clock), so it never relies on a same-day vendor delivery.

``--reuse`` (v8 C-3 contract): ``PRODUCERS``, ``HOST_HANDLES``, ``producer_group``, ``field_spec``, ``reuse_inputs``
and ``entry_inputs``; pins: the stage name and its manifest SHA-256, and the NYSE rule calendar of the previous-session
map (research_fields_price.session_calendar).
"""
from __future__ import annotations

import datetime as dt
import json
from pathlib import Path
import re
import sys

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

import research_fields_price as price  # same directory: the calendar pin only (session_calendar)
import research_window as rw
from research_fields_sec import _Host, nyse_sessions

GROUP = "gold"
OPTIONS = ("gold_panel_root", "gold_sha256", "gold_characteristics_sha256")
HOST_HANDLES = ("h",)
LAG_SESSIONS = 1                     # row t holds the panel value of the session before t
STAGES = {"gold": {"schema": "atx.alpha-panel.gold/v1", "pin": "gold_sha256"},
          "characteristics": {"schema": "atx.alpha-panel.characteristics/v2",
                              "pin": "gold_characteristics_sha256"}}
KEYS = ("session_date", "security_id")
ALLOW_ADMITTED_SCORES = False        # g_* / c_* are IC-selected on 2019-2021 labels: a PM ruling lifts this
FORBIDDEN_TOKEN = re.compile(r"^(label|labels|fwd|forward|future|ic|oos|holdout|target|outcome|lead|leads|ret\d+f)\d*$")
PARTITION = re.compile(r"^year=(\d{4})/[^/]+\.parquet$")
VALUE_TYPES = (pa.float32(), pa.float64())

GOLD_CLOCK = ("gold-panel-lag1-v1: row t holds the atx-db panel value of the session before t (NYSE rule session "
              "before the role's first session for row 0), a value atx-db stamps with information known by that "
              "session's 22:00 UTC mark; files bound by their stage manifest; nothing on or after the seal is read")


class GoldSelectionColumn(ValueError):
    """A gold ``g_*`` score or ``c_*`` composite: chosen by an IC screen on TRAIN-window labels."""


def refused_reason(stage: str, column: str) -> str | None:
    """Why a (stage, column) may not be read, or None. Label / forward / IC / holdout names and non-allowlisted
    stages are refused outright; admitted gold scores raise ``GoldSelectionColumn`` at declaration."""
    if stage not in STAGES:
        return f"stage {stage!r} is not a gold-pipeline feature stage ({', '.join(STAGES)})"
    if not re.fullmatch(r"[a-z][a-z0-9_]*", column) or column in KEYS:
        return f"column {column!r} is not a feature column"
    bad = [tok for tok in column.split("_") if FORBIDDEN_TOKEN.match(tok)]
    if bad:
        return f"column {column!r} carries a label / forward / IC / holdout token ({', '.join(bad)})"
    return None


FIELDS: dict = {}


def declare(name: str, stage: str, column: str, units: str, definition: str, caveats: list, formula: str) -> dict:
    """Add one field spec (the fields spec entry) after the refusals: a field name ``gp_*``, an allowlisted stage, a
    feature column without a forbidden token, and no admitted gold score unless ``ALLOW_ADMITTED_SCORES``."""
    if not re.fullmatch(r"gp_[a-z0-9_]+", name):
        raise ValueError(f"gold field {name!r}: names are gp_<column>")
    why = refused_reason(stage, column)
    if why:
        raise ValueError(f"gold field {name!r}: {why}; refusing")
    if stage == "gold" and column.startswith(("g_", "c_")) and not ALLOW_ADMITTED_SCORES:
        raise GoldSelectionColumn(f"gold field {name!r}: {column} is an IC-admitted gold score (selected on "
                                  "2019-2021 forward returns, inside TRAIN); binding it needs a PM ruling and a trial "
                                  "count; refusing")
    if name in FIELDS:
        raise ValueError(f"gold field {name!r} is already declared")
    spec = {"group": "gold_cols", "point_in_time": True, "lagged": False, "units": units, "clock": GOLD_CLOCK,
            "staleness": "no fill: a line-session absent from the stage, or a NaN value -> NaN",
            "caveats": caveats, "definition": definition, "source_columns": [*KEYS, column],
            "formula_id": formula, "min_history": "the stage's first session", "stage": stage, "column": column,
            "needs": "gold_panel"}
    FIELDS[name] = spec
    return spec


_SRC = ("atx-db vendor price / identity vintages are not certified (manifest source_vintages_certified false); "
        "repaired zero-ID lines are quarantined by the stage (null) for 1,260 sessions")
declare("gp_hl_spread_21", "gold", "ctl_hl_spread_21",
        "decimal spread: Corwin-Schultz (2012) high-low bid-ask spread estimate, 21-session mean",
        "row t = the gold stage's ctl_hl_spread_21 of the session before t: atx-db char_registry hl_spread_21 (mean "
        "two-day high-low spread estimate, floored at 0, over 21 sessions of TickerHistory3 high / low; a control "
        "shipped regardless of IC, raw units)",
        [_SRC, "member_equity rows only (the gold universe); a cost input, not a prior-signed signal"],
        "gold-ctl-hl-spread-21-lag1-v1")
declare("gp_iv_term_slope", "characteristics", "iv_term_slope",
        "annualized decimal: 252-day minus 21-day ATM implied volatility, both of the previous session",
        "row t = the characteristics stage's iv_term_slope of the session before t: atx-db char_registry iv_term_slope "
        "= iv_atm_252d(d-1) - iv_atm_21d(d-1) (TickerHistory3 atmCenI, earnings-censored; the v8 store has no 252-day "
        "tenor)", [_SRC, "vendor IV snapshot vintages unverified; atx-db already lags the IV one session"],
        "gold-char-iv-term-slope-lag1-v1")

# The producing code (contract of task C-3: {group: (entry names,)}); compute() orchestrates.
PRODUCERS = {"gold_cols": ("stage_manifest", "stage_partitions", "read_stage_columns", "previous_sessions",
                           "gold_rows")}


# ---------------------------------------------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------------------------------------------

def stage_manifest(h, root: Path, stage: str, pin: str) -> dict:
    """The stage's manifest, refused unless its SHA-256 is ``pin``, its schema the stage's and its status complete."""
    why = refused_reason(stage, "x")
    if why:
        raise ValueError(why)
    path = Path(root) / stage / "manifest.json"
    blob = path.read_bytes()
    if h.sha_bytes(blob) != str(pin).lower():
        raise ValueError(f"gold: {stage}/manifest.json SHA-256 differs from its pin")
    m = json.loads(blob)
    if m.get("schema") != STAGES[stage]["schema"] or m.get("status") != "complete":
        raise ValueError(f"gold: {stage} is not a complete {STAGES[stage]['schema']} publication")
    if not isinstance(m.get("files"), dict):
        raise ValueError(f"gold: {stage}/manifest.json lists no files")
    return {"manifest": m, "sha256": h.sha_bytes(blob), "path": str(path.resolve())}


def stage_partitions(files: dict, first_year: int, last_year: int) -> tuple:
    """([(year, rel, identity)], sealed partitions skipped): the manifest's year files in [first_year, last_year]; a
    partition that begins on or after the seal is never listed for opening."""
    out, sealed = [], 0
    for rel, ident in sorted(files.items()):
        hit = PARTITION.match(rel)
        if not hit:
            continue
        year = int(hit.group(1))
        if rw.partition_is_sealed(year):
            sealed += 1
            continue
        if first_year <= year <= last_year:
            out.append((year, rel, ident))
    return out, sealed


def assert_sealed(h, days: np.ndarray, what: str) -> None:
    """Refuse a read result holding a row on or after the seal (the pushed-down filter must have removed it)."""
    if len(days) and int(days.max()) >= h.day_of(rw.SEAL):
        raise rw.SealError(rw.seal_message(f"{what}: a row dated {h.date_of(int(days.max()))}"))


def read_stage_columns(h, root: Path, stage: str, man: dict, columns: list, lo_day: int, hi_day: int,
                       budget) -> tuple:
    """{column: (days, ids, values)} of the stage rows with lo_day <= session_date <= hi_day (< seal), read with the
    filter and the column projection pushed down; and the read statistics."""
    seal = rw.SEAL
    hi = min(hi_day, h.day_of(seal) - 1)
    lo_d, hi_d = dt.date(1970, 1, 1) + dt.timedelta(days=int(lo_day)), dt.date(1970, 1, 1) + dt.timedelta(days=hi)
    parts, sealed = stage_partitions(man["manifest"]["files"], lo_d.year, hi_d.year)
    st = {"stage": stage, "partitions_read": [], "sealed_partitions_never_opened": sealed,
          "rows_read": 0, "row_groups_pruned_on_or_after_seal": 0, "rows_in_pruned_row_groups": 0}
    got: dict = {c: ([], [], []) for c in columns}
    for year, rel, ident in parts:
        path = Path(root) / stage / rel
        captured = h.identity(path)
        if captured[2] != ident.get("bytes") or h.sha_file(path, budget) != ident.get("sha256"):
            raise ValueError(f"gold: {stage}/{rel} differs from its manifest entry")
        pf = pq.ParquetFile(path, memory_map=False)
        schema = pf.schema_arrow
        need = {"session_date": (pa.date32(),), "security_id": (pa.int64(),), **{c: VALUE_TYPES for c in columns}}
        for c, types in need.items():
            i = schema.get_field_index(c)
            if i < 0 or schema.field(i).type not in types:
                raise ValueError(f"gold: {stage}/{rel} column {c} is missing or not {' / '.join(map(str, types))}")
        di = schema.get_field_index("session_date")
        for g in range(pf.metadata.num_row_groups):          # statistics only: what the filter skips
            stats = pf.metadata.row_group(g).column(di).statistics
            if stats is not None and stats.has_min_max and stats.min >= seal:
                st["row_groups_pruned_on_or_after_seal"] += 1
                st["rows_in_pruned_row_groups"] += pf.metadata.row_group(g).num_rows
        table = pq.read_table(path, columns=[*KEYS, *columns], use_threads=False, memory_map=False,
                              filters=[("session_date", "<", seal), ("session_date", ">=", lo_d),
                                       ("session_date", "<=", hi_d)])
        if h.identity(path) != captured:
            raise ValueError(f"gold: {stage}/{rel} changed while it was read")
        days = table.column("session_date").cast(pa.int32()).to_numpy(zero_copy_only=False).astype(np.int64)
        assert_sealed(h, days, f"gold {stage}/{rel}")
        ids = table.column("security_id").to_numpy(zero_copy_only=False).astype(np.int64)
        for c in columns:
            v = table.column(c).to_numpy(zero_copy_only=False).astype(np.float64)
            got[c][0].append(days)
            got[c][1].append(ids)
            got[c][2].append(v)
        st["partitions_read"].append(rel)
        st["rows_read"] += table.num_rows
        budget.check("gold-partition")
    out = {c: tuple(np.concatenate(x) if x else np.zeros(0) for x in got[c]) for c in columns}
    return out, st


def previous_sessions(h, role) -> np.ndarray:
    """Epoch day of the session before each role session: role.days[t - 1], and for row 0 the NYSE rule session
    before the role's first session (research_fields_sec.nyse_sessions)."""
    if LAG_SESSIONS not in (0, 1):
        raise ValueError("internal: LAG_SESSIONS must be 0 or 1")
    first = int(role.days[0])
    epoch = dt.date(1970, 1, 1)
    before = nyse_sessions(epoch + dt.timedelta(days=first - 14), epoch + dt.timedelta(days=first - 1))
    if not len(before):
        raise ValueError("internal: no NYSE rule session before the role")
    days = np.concatenate(([int(before[-1])], role.days.astype(np.int64)))
    return days[1 - LAG_SESSIONS: len(days) - LAG_SESSIONS]


# ---------------------------------------------------------------------------------------------------------------
# Producer
# ---------------------------------------------------------------------------------------------------------------

def gold_rows(h, name: str, data: tuple, prev: np.ndarray, role, output: Path, budget) -> tuple:
    """Write field ``name`` (row t = the value dated prev[t]); duplicate keys are quarantined (NaN). Returns (writer,
    extras)."""
    days, ids, values = data
    t = np.searchsorted(prev, days)
    on = (t < len(prev)) & (prev[np.minimum(t, len(prev) - 1)] == days)
    j, known = role.columns_of(ids)
    keep = on & known
    mat = np.full((role.n_dates, role.n), np.nan)
    count = np.zeros((role.n_dates, role.n), dtype=np.uint16)
    np.add.at(count, (t[keep], j[keep]), 1)
    mat[t[keep], j[keep]] = values[keep]
    dup = count > 1
    mat[dup] = np.nan
    w = h.FieldWriter(output, name, role)
    try:
        for r in range(role.n_dates):
            w.write(mat[r])
            if r % 256 == 0:
                budget.check("gold-write")
    except BaseException:
        w.f.close()
        raise
    w.close()
    return w, {"rows_off_axis_or_unknown_line": int(np.count_nonzero(~keep)),
               "duplicate_keys_quarantined": int(np.count_nonzero(dup))}


# ---------------------------------------------------------------------------------------------------------------
# The module object and the --reuse interface (v8 C-3)
# ---------------------------------------------------------------------------------------------------------------

def bind(host_namespace: dict) -> "GoldFieldModule":
    registry = host_namespace["ALL_FIELDS"]
    clash = [x for x in FIELDS if x in registry and registry[x] is not FIELDS[x]]
    if clash:
        raise ValueError(f"research_fields_gold: {', '.join(clash)} already registered by another producer")
    registry.update(FIELDS)
    return GoldFieldModule(host_namespace)


def producer_group(name: str) -> str:
    return FIELDS[name]["group"]


def field_spec(name: str) -> dict:
    return FIELDS[name]


def reuse_inputs(name: str, options: dict) -> dict:
    stage = FIELDS[name]["stage"]
    return {"stage": stage, "stage_manifest_sha256": str(options.get(STAGES[stage]["pin"]) or "").lower(),
            "session_calendar": price.session_calendar()}


def entry_inputs(entry: dict) -> dict:
    src = entry.get("gold_stage") if isinstance(entry.get("gold_stage"), dict) else {}
    return {"stage": src.get("stage"), "stage_manifest_sha256": src.get("manifest_sha256"),
            "session_calendar": entry.get("session_calendar")}


class GoldFieldModule:
    GROUP, FIELDS, OPTIONS = GROUP, FIELDS, OPTIONS

    def __init__(self, host_namespace: dict):
        self.h = _Host(host_namespace)

    @staticmethod
    def add_arguments(parser):
        g = parser.add_argument_group("gold alpha-panel fields (research_fields_gold.py: " + ",".join(FIELDS) + ")")
        g.add_argument("--gold-panel-root", type=Path, help="the atx-db alpha-panel build root (read only)")
        g.add_argument("--gold-sha256", help="SHA-256 of <root>/gold/manifest.json")
        g.add_argument("--gold-characteristics-sha256", help="SHA-256 of <root>/characteristics/manifest.json")

    def check(self, selected, options: dict):
        """Before any output: the root, each needed stage's pin, its manifest (pin, schema, status)."""
        names = [f for f in selected if f in FIELDS]
        if not names:
            return
        for x in names:
            why = refused_reason(FIELDS[x]["stage"], FIELDS[x]["column"])
            if why:
                raise ValueError(f"{x}: {why}")
        if options.get("gold_panel_root") is None:
            raise ValueError(f"--fields: {', '.join(names)} need --gold-panel-root (the atx-db alpha-panel build root)")
        for stage in sorted({FIELDS[x]["stage"] for x in names}):
            pin = options.get(STAGES[stage]["pin"])
            if not pin:
                raise ValueError(f"--fields: the {stage} stage needs --{STAGES[stage]['pin'].replace('_', '-')}")
            stage_manifest(self.h, Path(options["gold_panel_root"]), stage, pin)

    def compute(self, names, role, output: Path, budget, options: dict, outcome: dict, source_checks: dict,
                field_extras: dict):
        if not names:
            return
        h = self.h
        if h.SEAL != rw.SEAL:
            raise rw.SealError(f"research_fields_gold: the builder's seal {h.SEAL.isoformat()} is not research_window's "
                               f"{rw.SEAL_DATE} ({rw.WINDOW_ID}); refusing (sealed)")
        root = Path(options["gold_panel_root"])
        prev = previous_sessions(h, role)
        budget.admit(len(names) * role.n_dates * role.n * 10 + (32 << 20), "gold-fields")
        checks = {"lag_sessions": LAG_SESSIONS, "seal": rw.SEAL_DATE, "window": rw.WINDOW_ID, "stages": {}}
        producer = {"module": Path(__file__).name, **h.module_code_identity(sys.modules[__name__])}
        calendar = price.session_calendar()
        for stage in sorted({FIELDS[x]["stage"] for x in names}):
            these = [x for x in names if FIELDS[x]["stage"] == stage]
            man = stage_manifest(h, root, stage, options[STAGES[stage]["pin"]])
            data, st = read_stage_columns(h, root, stage, man, [FIELDS[x]["column"] for x in these],
                                          int(prev[0]), int(prev[-1]), budget)
            checks["stages"][stage] = {"manifest_sha256": man["sha256"], "schema": STAGES[stage]["schema"], **st}
            for x in these:
                spec = FIELDS[x]
                w, extra = gold_rows(h, x, data[spec["column"]], prev, role, output, budget)
                src = [{"path": man["path"], "sha256": man["sha256"], "stage": stage}]
                field_extras[x] = {"producer": producer, "formula_id": spec["formula_id"],
                                   "formula_sha256": h.formula_id(x, h.spec_definition(x, LAG_SESSIONS)),
                                   "lag_sessions": LAG_SESSIONS, "min_history": spec["min_history"],
                                   "gold_stage": {"stage": stage, "manifest_sha256": man["sha256"],
                                                  "column": spec["column"]},
                                   "session_calendar": dict(calendar), **extra}
                outcome[x] = (w, src, w.coverage())
            del data
        source_checks[GROUP] = checks
        budget.report("gold-complete", fields=len(names))
