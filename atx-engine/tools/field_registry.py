"""The research field registry (contract K-P9-1, lane A1 of P9): ``field_registry.json`` and the one builder entry.

  python atx-engine/tools/prepare_research_fields.py --registry atx-engine/tools/field_registry.json
      [--fields <name,name,...|all>] [--engine-exe <build>/bin/atx-research-fields.exe] <the builder's other argv>

``field_registry.json`` (schema ``atx.field-registry/v1``) lists every producible research field, one row per field, in
registration order, which is the order of the fields in a fields manifest. It was generated once (``generate``) from the
four registration mechanisms of the base commit d7c1c520, which the registry replaces:
(a) the builder's inline dicts ``FIELDS``, ``ISSUER_FIELDS``, ``SV_FIELDS``;
(b) the ``FIELD_MODULES`` hooks, both the builder's own (SEC, price, v8) and the four draft shims'
    (``prepare_research_fields_{draft,xdata,ohlc,ydata}.py``, in the order of the fields v13+ one-liner);
(c) the holdings wrap of ``run`` / ``main``, now a late ``FIELD_MODULES`` module (its fields come last);
(d) the engine shim (``prepare_research_fields_engine.py``), which routes si_shares, si_dtc and vol_126 to the C++
    executable.
Each row has exactly ``ROW_KEYS``:

* ``name``; ``kind`` ``python`` or ``engine``; ``builder``: for a python row, the Python module that computes the field
  (module name, no ``.py``); for an engine row, the C++ ``BuilderKind`` id (lane A2);
* ``dtype`` ``f64`` or ``group``, declared by ``DTYPE_RULE`` (never inferred from a ``grp_`` name prefix);
* ``point_in_time``; ``spec_text``: the field's spec-level definition (``spec_text``, the builder's ``spec_definition``
  keys) at the declared fundamentals lag; ``formula_sha256``: the builder's ``formula_id`` of that text (equal to the
  engine's formula fingerprint for a ported field);
* ``requires``: fields that must be in the same run; ``options``: ``{"group": <producer group>}`` for a python row (the
  group that --reuse fingerprints); ``sources``: the input names the producer reads (``run()`` keywords or the module
  options' destinations); ``first_session``: not declared yet (null); ``owner``: the registration mechanism.

A ``kind: engine`` row is either the engine twin of a field the Python builder also produces (a port: si_shares,
si_dtc, vol_126) or an engine-only row, which the bound builder cannot produce: no Python twin. An engine-only row is
how DEC-5 adds every new field: a row naming its C++ builder kind, added to the file with no edit here. Only the
Python-producible rows are compared with the code (``check``) and regenerated from it (``regenerate``); an engine-only
row is checked by the schema alone (``validate``) and kept verbatim at its position.

The entry (``entry``, called by the builder's ``main`` when ``--registry`` is given) loads and validates the registry,
binds every module a python row names into the builder (rows in registry order), refuses a registry whose
Python-producible rows disagree with the code (their field set and relative order, owner module, point-in-time flag,
dependencies, dtype rule, formula fingerprint) or that has a python row the code cannot produce, expands ``--fields
all`` to every row, and runs the builder's own argv parser and ``run``. Engine rows are computed by the executable only
when ``--engine-exe`` is given (``prepare_research_fields_engine.engine_path``); without it the Python builder computes
an engine twin, as before (the engine path becomes the default only after root's TRAIN identity run, migration section
3.3). A requested engine-only row has no Python fallback: the entry refuses it without ``--engine-exe``, and with it
unless the engine shim routes it (``ENGINE_FIELDS``), before any output. With ``--registry`` absent nothing changes: the
builder's ``main`` never calls this module.
"""
from __future__ import annotations

import argparse
import copy
import importlib
import json
from pathlib import Path
import re
import sys

SCHEMA = "atx.field-registry/v1"
DEFAULT_PATH = Path(__file__).resolve().with_name("field_registry.json")
ROW_KEYS = ("name", "kind", "builder", "dtype", "point_in_time", "spec_text", "formula_sha256", "requires", "options",
            "sources", "first_session", "owner")
KINDS = ("python", "engine")
DTYPES = ("f64", "group")
GROUP_UNITS = "categorical code"
DTYPE_RULE = (f"dtype is group iff the field's declared units begin with '{GROUP_UNITS}' (a classifier: the VM groups "
              "by its value), else f64; declared here, never inferred from a grp_ name prefix (DS review section 2)")
BUILDER = "prepare_research_fields"
# The draft shims of the base commit, in the order of the fields v13+ one-liner (ohlc shim docstring; v8y prereg 9).
SHIMS = ("prepare_research_fields_draft", "prepare_research_fields_xdata", "prepare_research_fields_ohlc",
         "prepare_research_fields_ydata")
ENGINE_SHIM = "prepare_research_fields_engine"
BUILDER_DICTS = ("FIELDS", "ISSUER_FIELDS", "SV_FIELDS")
GROUP_SOURCES = {"role": ["role"], "finra": ["finra"], "th": ["tickerhistory"], "lake": ["lake"],
                 "finra_sv": ["finra_short_volume", "tickerhistory"]}
ISSUER_SOURCES = {"fund": ["identity_bridge", "fund_events"], "me": ["identity_bridge", "role"],
                  "grp": ["identity_bridge", "fund_events", "sic_events"]}
SOURCE_OPTION_EXCLUDED = re.compile(r"(_sha256|^fund_lag_sessions)$")
HEX64 = re.compile(r"[0-9a-f]{64}")


class RegistryError(ValueError):
    """The registry file is malformed or disagrees with the builder's code."""


# ---------------------------------------------------------------------------------------------------------------------
# The file
# ---------------------------------------------------------------------------------------------------------------------

def validate(doc) -> dict:
    """``doc`` when it is a well-formed ``atx.field-registry/v1`` document, else RegistryError."""
    if not isinstance(doc, dict) or doc.get("schema") != SCHEMA or not isinstance(doc.get("fields"), list):
        raise RegistryError(f"field registry: not an {SCHEMA} document with a fields list")
    seen = set()
    for i, row in enumerate(doc["fields"]):
        where = f"field registry row {i}"
        if not isinstance(row, dict) or set(row) != set(ROW_KEYS):
            raise RegistryError(f"{where}: needs exactly the keys {', '.join(ROW_KEYS)}")
        name = row["name"]
        if not isinstance(name, str) or not name or name in seen:
            raise RegistryError(f"{where}: name must be a new non-empty string")
        where = f"field registry row {name}"
        if row["kind"] not in KINDS or row["dtype"] not in DTYPES or not isinstance(row["point_in_time"], bool):
            raise RegistryError(f"{where}: kind in {KINDS}, dtype in {DTYPES}, point_in_time a bool")
        if not isinstance(row["builder"], str) or not row["builder"] or not isinstance(row["owner"], str):
            raise RegistryError(f"{where}: builder and owner must be strings")
        if row["formula_sha256"] is not None and not (isinstance(row["formula_sha256"], str)
                                                      and HEX64.fullmatch(row["formula_sha256"])):
            raise RegistryError(f"{where}: formula_sha256 must be 64 lower-case hex digits or null")
        for key in ("requires", "sources"):
            if not isinstance(row[key], list) or not all(isinstance(x, str) for x in row[key]):
                raise RegistryError(f"{where}: {key} must be a list of strings")
        missing = [x for x in row["requires"] if x not in seen]
        if missing:
            raise RegistryError(f"{where}: requires {', '.join(missing)}, which no earlier row registers")
        if not isinstance(row["options"], dict) or row["first_session"] is not None and not isinstance(
                row["first_session"], str):
            raise RegistryError(f"{where}: options must be an object and first_session a string or null")
        seen.add(name)
    return doc


def load(path: Path | str | None = None) -> dict:
    """The validated registry document (default: ``field_registry.json`` beside this file)."""
    p = Path(DEFAULT_PATH if path is None else path)
    try:
        doc = json.loads(p.read_text(encoding="utf-8"))
    except (OSError, ValueError) as err:
        raise RegistryError(f"field registry {p}: {err}") from None
    return validate(doc)


def dump(doc: dict) -> bytes:
    """The committed byte form: sorted keys inside rows, rows in registry order, two-space indent, LF."""
    return (json.dumps(validate(doc), indent=2, sort_keys=True, allow_nan=False) + "\n").encode("utf-8")


def names(doc: dict) -> list:
    return [row["name"] for row in doc["fields"]]


def select(doc: dict, fields: str) -> list:
    """``--fields``: ``all`` (every row, registry order) or comma-separated names that are rows of the registry."""
    known = names(doc)
    if fields.strip() == "all":
        return known
    want = [x.strip() for x in fields.split(",") if x.strip()]
    unknown = [x for x in want if x not in known]
    if not want or unknown:
        raise RegistryError(f"--fields: {', '.join(unknown) or 'nothing'} not in the field registry")
    return want


# ---------------------------------------------------------------------------------------------------------------------
# Binding modules into the builder namespace
# ---------------------------------------------------------------------------------------------------------------------

def bind_modules(ns: dict, modules) -> list:
    """Bind each field module into the builder namespace ``ns`` (``vars(prepare_research_fields)``) exactly as the
    builder binds its own (``bind`` registers the module's fields; the module object joins ``FIELD_MODULES``); a module
    already registered there is left as it is. Returns the bound module objects (the four draft shims' ``register``)."""
    registered = ns["FIELD_MODULES"]
    bound = []
    for module in modules:
        present = [m for m in registered if type(m).__module__ == module.__name__]
        if present:
            bound += present
            continue
        m = module.bind(ns)
        registered.append(m)
        bound.append(m)
    return bound


def python_modules(doc: dict) -> list:
    """The field modules the rows need bound, in first-row order (the builder itself excluded): a python row's
    ``builder``; an engine row's ``builder`` is a C++ kind id, never a module, so its Python module (the fallback without
    --engine-exe) comes from its ``owner``: a draft shim's modules, or nothing for a field the builder binds itself."""
    out = []
    for row in doc["fields"]:
        if row["kind"] == "python":
            names = [row["builder"]] if row["builder"] != BUILDER else []
        else:
            mechanism = row["owner"].split(";")[0].strip()
            shim = mechanism[len("shim:"):] if mechanism.startswith("shim:") else None
            names = [m.__name__ for m in importlib.import_module(shim).DRAFT_MODULES] if shim in SHIMS else []
        out += [x for x in names if x not in out]
    return out


def bind(ns: dict, doc: dict) -> list:
    """Import and bind every module a python row names (registry order). Returns the bound module objects."""
    return bind_modules(ns, [importlib.import_module(x) for x in python_modules(doc)])


# ---------------------------------------------------------------------------------------------------------------------
# The code's registry (what the bound builder can produce) and its rows
# ---------------------------------------------------------------------------------------------------------------------

def late_modules(ns: dict) -> list:
    """Bound modules whose fields the builder computes when it assembles the manifest, after every other field."""
    return [m for m in ns["FIELD_MODULES"] if getattr(m, "LATE", False)]


def code_fields(ns: dict) -> dict:
    """{name: (spec, module name)} of every field the bound builder can produce, in manifest order: ``ALL_FIELDS``
    order, then each late module's fields."""
    owner: dict = {}
    for m in ns["FIELD_MODULES"]:
        for x in m.FIELDS:
            owner.setdefault(x, type(m).__module__)
    out = {x: (spec, owner.get(x, BUILDER)) for x, spec in ns["ALL_FIELDS"].items()}
    for m in late_modules(ns):
        out.update({x: (spec, type(m).__module__) for x, spec in m.FIELDS.items()})
    return out


def spec_text(spec: dict, lag: int) -> dict:
    """A field's spec-level definition: the builder's ``spec_definition`` keys (units, clock at the declared lag,
    staleness, source columns, definition, point-in-time flags, domain) from its registry spec."""
    clock = spec["clock"].format(lag=lag) if spec.get("lagged") else spec["clock"]
    return {"units": spec["units"], "clock": clock, "staleness": spec["staleness"],
            "source_columns": spec["source_columns"], "definition": spec.get("definition"),
            "point_in_time": spec["point_in_time"], "non_pit_aspects": spec.get("non_pit_aspects", []),
            "domain": [float(x) for x in spec["domain"]] if "domain" in spec else None}


def dtype_of(spec: dict) -> str:
    return "group" if str(spec["units"]).startswith(GROUP_UNITS) else "f64"


def producer_group(name: str, spec: dict, module: str) -> str:
    """The group --reuse fingerprints the field's producer under (a builder group or the module's own)."""
    if module == BUILDER:
        return spec["group"]
    return sys.modules[module].producer_group(name)


def sources_of(spec: dict, module: str) -> list:
    """The input names a field's producer reads besides the role axes and the fields it requires."""
    if module == BUILDER:
        if spec["group"] == "issuer":
            return list(ISSUER_SOURCES[spec["kind"]])
        return list(GROUP_SOURCES[spec["group"]])
    mod = sys.modules[module]
    if module == "research_fields_price":   # role payloads, this run's fields, or the vendor file (spec "needs")
        return {"role": ["role"], "fields": []}.get(spec["needs"], ["price_source"])
    if module == "research_fields_holdings":   # the stages of the field's kind
        return list(mod.KIND_STAGES[spec["kind"]])
    return [k for k in mod.OPTIONS if not SOURCE_OPTION_EXCLUDED.search(k)]


def owners(ns: dict) -> dict:
    """{module name: registration mechanism} of the base commit (``owner`` of the generated rows)."""
    out = {x: f"builder:{x}" for x in BUILDER_DICTS}
    for shim in SHIMS:
        for module in importlib.import_module(shim).DRAFT_MODULES:
            out[module.__name__] = f"shim:{shim}"
    for m in ns["FIELD_MODULES"]:
        out.setdefault(type(m).__module__, "builder:FIELD_MODULES")
    return out


def engine_flips(doc: dict) -> dict:
    """{name: C++ builder kind id} of a registry's engine rows: ``generate(ns, engine=...)`` reproduces the engine twins
    among them; ``regenerate`` also keeps the engine-only ones."""
    return {row["name"]: row["builder"] for row in doc["fields"] if row["kind"] == "engine"}


def generate(ns: dict, engine: dict | None = None) -> dict:
    """The registry of a builder namespace in which every shim's modules are bound (``bind_modules``): one row per
    producible field, in manifest order; python rows, except ``engine`` ({name: builder kind id}, e.g. lane A2's flip of
    si_shares, si_dtc, vol_126), whose rows get ``kind: engine`` and that id as ``builder`` (nothing else moves). Used
    once to write ``field_registry.json``; tests re-run it."""
    ported = importlib.import_module(ENGINE_SHIM).ENGINE_FIELDS
    lag = ns["FUND_LAG_SESSIONS_DECLARED"]
    mech = owners(ns)
    rows = []
    for name, (spec, module) in code_fields(ns).items():
        owner = mech[next(d for d in BUILDER_DICTS if name in ns[d])] if module == BUILDER else mech[module]
        if name in ported:
            owner += f"; engine-shim:{ENGINE_SHIM}"
        text = spec_text(spec, lag)
        rows.append({"name": name, "kind": "python", "builder": module, "dtype": dtype_of(spec),
                     "point_in_time": bool(spec["point_in_time"]), "spec_text": text,
                     "formula_sha256": ns["formula_id"](name, text), "requires": list(spec.get("requires", [])),
                     "options": {"group": producer_group(name, spec, module)},
                     "sources": sources_of(spec, module), "first_session": None, "owner": owner})
    flips = dict(engine or {})
    unknown = sorted(set(flips) - {row["name"] for row in rows})
    if unknown:
        raise RegistryError(f"generate: engine rows {', '.join(unknown)} are not producible fields")
    for row in rows:
        if row["name"] in flips:
            row.update(kind="engine", builder=flips[row["name"]])
    return validate({"schema": SCHEMA, "dtype_rule": DTYPE_RULE, "fields": rows})


def engine_only(ns: dict, doc: dict) -> list:
    """Names of the registry's engine rows the bound builder cannot produce (no Python twin), in registry order."""
    code = code_fields(ns)
    return [row["name"] for row in doc["fields"] if row["kind"] == "engine" and row["name"] not in code]


def regenerate(ns: dict, doc: dict) -> dict:
    """The generator round-trip of a registry ``doc`` that may hold engine-only rows (DEC-5): ``generate`` with
    ``doc``'s engine twins flipped as in ``doc``, its rows filling ``doc``'s Python-producible positions in code order,
    and ``doc``'s engine-only rows kept verbatim at their positions (a python row the code cannot produce is dropped;
    a code field without a row is appended). ``dump(regenerate(ns, doc)) == dump(doc)`` exactly when ``doc`` is the
    code's registry with valid engine-only rows spliced in; with none it equals ``generate(ns, engine_flips(doc))``."""
    code = code_fields(ns)
    flips = {name: kind_id for name, kind_id in engine_flips(doc).items() if name in code}
    gen = generate(ns, engine=flips)
    generated = iter(gen["fields"])
    rows = [next(generated) if row["name"] in code else copy.deepcopy(row) for row in doc["fields"]
            if row["name"] in code or row["kind"] == "engine"]
    return validate({**gen, "fields": rows + list(generated)})


def check(ns: dict, doc: dict) -> None:
    """Refuse a registry whose Python-producible rows disagree with the bound builder: the same fields in the same
    relative order, and per row the owning module (python rows), point-in-time flag, dependencies, dtype and formula
    fingerprint. A python row the builder cannot produce is refused; an engine-only row (no Python twin, DEC-5) is
    accepted as ``validate`` left it."""
    code = code_fields(ns)
    lag = ns["FUND_LAG_SESSIONS_DECLARED"]
    producible = [x for x in names(doc) if x in code]
    extra = [row["name"] for row in doc["fields"] if row["name"] not in code and row["kind"] != "engine"]
    if extra or producible != list(code):
        absent = [x for x in code if x not in producible]
        raise RegistryError("field registry and builder disagree on the field list or its order"
                            + (f" (rows the builder cannot produce: {', '.join(extra)})" if extra else "")
                            + (f" (fields without a row: {', '.join(absent)})" if absent else ""))
    for row in doc["fields"]:
        name = row["name"]
        if name not in code:   # engine-only: the executable's row; nothing in Python to compare it with
            continue
        spec, module = code[name]
        if row["kind"] == "python" and row["builder"] != module:
            raise RegistryError(f"field registry row {name}: builder {row['builder']} but {module} produces it")
        if (row["point_in_time"] != bool(spec["point_in_time"]) or row["requires"] != list(spec.get("requires", []))
                or row["dtype"] != dtype_of(spec)):
            raise RegistryError(f"field registry row {name}: point_in_time, requires or dtype differ from the code")
        if row["formula_sha256"] is not None and row["formula_sha256"] != ns["formula_id"](name,
                                                                                         spec_text(spec, lag)):
            raise RegistryError(f"field registry row {name}: formula_sha256 differs from the code's spec text")


# ---------------------------------------------------------------------------------------------------------------------
# The entry
# ---------------------------------------------------------------------------------------------------------------------

def wants_registry(argv) -> bool:
    return any(a == "--registry" or a.startswith("--registry=") for a in argv)


def entry(ns: dict, argv: list, build_main) -> object:
    """``prepare_research_fields.py --registry R --fields <list|all> [--engine-exe EXE] ...``: bind, check, expand,
    then ``build_main`` (the builder's argv parser and run) on the remaining argv with the expanded ``--fields``."""
    p = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    p.add_argument("--registry", type=Path, required=True)
    p.add_argument("--fields")
    p.add_argument("--engine-exe", type=Path)
    a, rest = p.parse_known_args(argv)
    doc = load(a.registry)
    bind(ns, doc)
    check(ns, doc)
    # no --fields: the builder's default list (its point-in-time inline fields), as without the registry
    fields = select(doc, a.fields) if a.fields is not None else list(ns["DEFAULT_FIELDS"])
    kinds = {row["name"]: row["kind"] for row in doc["fields"]}
    engine_rows = [x for x in fields if kinds[x] == "engine"]
    no_twin = set(engine_only(ns, doc))
    only = [x for x in engine_rows if x in no_twin]   # engine-only rows: no Python fallback
    if a.fields is not None:
        rest = rest + ["--fields", ",".join(fields)]
    if engine_rows and a.engine_exe is not None:
        engine = importlib.import_module(ENGINE_SHIM)
        unrouted = [x for x in only if x not in engine.ENGINE_FIELDS]
        if unrouted:
            raise RegistryError(f"field registry: engine-only rows {', '.join(unrouted)} have no route in "
                                f"{ENGINE_SHIM} (it routes {', '.join(engine.ENGINE_FIELDS)}) and no Python "
                                "producer; refusing")
        with engine.engine_path(a.engine_exe, engine_rows):
            return build_main(rest)
    if only:
        raise RegistryError(f"field registry: engine-only rows {', '.join(only)} have no Python producer; they need "
                            "--engine-exe <build>/bin/atx-research-fields.exe")
    if engine_rows:
        print(f"field registry: engine rows {', '.join(engine_rows)} are computed by the Python builder "
              "(no --engine-exe)", file=sys.stderr, flush=True)
    elif a.engine_exe is not None:
        raise RegistryError("--engine-exe given but no requested row is of kind engine")
    return build_main(rest)
