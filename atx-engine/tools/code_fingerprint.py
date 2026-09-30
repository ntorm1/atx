"""Fingerprints of producing code: SHA-256 of the AST closure of named entry points in one Python source.

Generic machinery shared by the research field builder (``prepare_research_fields.py --reuse``), the fitter's and the
report card's record stores (``fit_composition_weights.py``, ``alpha_report_card.py``). A fingerprint changes when the
code that computes a value changes and only then:

* the closure starts at the entry names and follows every module-level definition (function, class, constant, import)
  that a reached statement reads by name, transitively; names listed in ``orchestration`` are never followed;
* each reached statement is hashed as its AST without docstrings and without positions, so comments, blank lines,
  formatting and docstrings do not count, while any code, constant or import change of a reached name does;
* a name that is bound more than once at module level contributes every binding statement.

``fingerprints(source, {group: (entry names,)})`` returns ``{group: sha256 | None}`` (None: the source lacks an entry
name); its values are the builder's historical ``producer_fingerprints`` values, bit for bit.
"""
from __future__ import annotations

import ast
import copy
import hashlib
import json

SCOPES = (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef, ast.Lambda, ast.ListComp, ast.SetComp, ast.DictComp,
          ast.GeneratorExp)


def canonical(value) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def bound_names(stmt) -> set:
    """Module-level names a top-level statement binds or mutates (a store, an item/attribute store or a method call
    on the name); nested scopes (defs, lambdas, comprehensions) bind their own names."""
    if isinstance(stmt, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
        return {stmt.name}
    if isinstance(stmt, (ast.Import, ast.ImportFrom)):
        return {(a.asname or a.name).split(".")[0] for a in stmt.names}
    names, todo = set(), [stmt]
    while todo:
        n = todo.pop()
        if isinstance(n, SCOPES):
            continue
        base = None
        if isinstance(n, ast.Name) and isinstance(n.ctx, ast.Store):
            names.add(n.id)
        elif isinstance(n, (ast.Subscript, ast.Attribute)) and isinstance(n.ctx, ast.Store):
            base = n.value
        elif isinstance(n, ast.Expr) and isinstance(n.value, ast.Call) and isinstance(n.value.func, ast.Attribute):
            base = n.value.func.value
        while isinstance(base, (ast.Subscript, ast.Attribute)):
            base = base.value
        if isinstance(base, ast.Name):
            names.add(base.id)
        todo.extend(ast.iter_child_nodes(n))
    return names


def free_names(stmt) -> set:
    """Names a top-level statement reads (for a def or class: minus the names bound inside it)."""
    loads = {n.id for n in ast.walk(stmt) if isinstance(n, ast.Name) and isinstance(n.ctx, ast.Load)}
    if not isinstance(stmt, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
        return loads
    local = set()
    for n in ast.walk(stmt):
        if isinstance(n, ast.Name) and isinstance(n.ctx, (ast.Store, ast.Del)):
            local.add(n.id)
        elif isinstance(n, ast.arg):
            local.add(n.arg)
        elif isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)) and n is not stmt:
            local.add(n.name)
        elif isinstance(n, (ast.Import, ast.ImportFrom)):
            local.update((a.asname or a.name).split(".")[0] for a in n.names)
        elif isinstance(n, ast.ExceptHandler) and n.name:
            local.add(n.name)
    return loads - local


def code_dump(stmt) -> str:
    """The statement's AST without docstrings (no positions: comments and formatting do not count)."""
    node = copy.deepcopy(stmt)
    for n in ast.walk(node):
        if (isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)) and n.body
                and isinstance(n.body[0], ast.Expr) and isinstance(n.body[0].value, ast.Constant)
                and isinstance(n.body[0].value.value, str)):
            n.body = n.body[1:] or [ast.Pass()]
    return ast.dump(node)


class Module:
    """One parsed source: module-level bindings and a cache of statement dumps."""

    def __init__(self, source: bytes):
        try:
            tree = ast.parse(source.decode("utf-8"))
        except (SyntaxError, UnicodeDecodeError, ValueError) as e:
            raise ValueError(f"builder source does not parse ({type(e).__name__})") from None
        self.bindings: dict = {}
        for stmt in tree.body:
            for name in bound_names(stmt):
                self.bindings.setdefault(name, []).append(stmt)
        self._dumps: dict = {}

    def has(self, names) -> bool:
        return all(n in self.bindings for n in names)

    def reach(self, entries, orchestration=frozenset()) -> list:
        """Sorted module-level names reached from ``entries`` (orchestration names are never followed)."""
        seen, todo = set(), list(entries)
        while todo:
            name = todo.pop()
            if name in seen or name in orchestration or name not in self.bindings:
                continue
            seen.add(name)
            for stmt in self.bindings[name]:
                todo.extend(free_names(stmt))
        return sorted(seen)

    def statements(self, names) -> list:
        """The binding statements of ``names`` (each once, in name order)."""
        out, ids = [], set()
        for name in names:
            for stmt in self.bindings[name]:
                if id(stmt) not in ids:
                    ids.add(id(stmt))
                    out.append(stmt)
        return out

    def closure(self, names) -> list:
        """[[name, [statement dumps]]] of ``names`` (the hashed form)."""
        out = []
        for name in names:
            for stmt in self.bindings[name]:
                if id(stmt) not in self._dumps:
                    self._dumps[id(stmt)] = code_dump(stmt)
            out.append([name, [self._dumps[id(s)] for s in self.bindings[name]]])
        return out


class Host:
    """The source a field module is bound into (the builder) and the handles through which the module reads it:
    ``h.X`` / ``self.h.X`` (attribute handles) and ``ns["X"]`` / ``ctx.ns["X"]`` (subscript handles)."""

    def __init__(self, source: bytes, handles: tuple, orchestration=frozenset()):
        self.module, self.handles, self.orchestration = Module(source), frozenset(handles), orchestration


def _is_handle(node, handles) -> bool:
    return ((isinstance(node, ast.Name) and node.id in handles) or
            (isinstance(node, ast.Attribute) and node.attr in handles))


def host_names(statements, handles) -> set:
    """Names the statements read from the host through a handle: ``<handle>.X`` and ``<handle>["X"]``."""
    names = set()
    for stmt in statements:
        for n in ast.walk(stmt):
            if isinstance(n, ast.Attribute) and _is_handle(n.value, handles):
                names.add(n.attr)
            elif (isinstance(n, ast.Subscript) and _is_handle(n.value, handles) and isinstance(n.slice, ast.Constant)
                  and isinstance(n.slice.value, str)):
                names.add(n.slice.value)
    return names


def fingerprints(source: bytes, producers: dict, orchestration=frozenset(), host: Host | None = None) -> dict:
    """{group: SHA-256 of its producing code} over one source: each group's entry names and every module-level
    definition they reach by name. With ``host`` (a field module bound into the builder) the hashed document also
    holds the host closure of every host name the group's statements read through a handle. None for a group whose
    entry names the source lacks; ValueError when a source does not parse."""
    module = Module(source)
    out = {}
    for group, entries in producers.items():
        if not module.has(entries):
            out[group] = None
            continue
        names = module.reach(entries, orchestration)
        doc = {"group": group, "closure": module.closure(names)}
        if host is not None:
            wanted = sorted(host_names(module.statements(names), host.handles))
            doc["host_closure"] = host.module.closure(host.module.reach(wanted, host.orchestration))
        out[group] = hashlib.sha256(canonical(doc).encode("utf-8")).hexdigest()
    return out


def fingerprint(source: bytes, entries: tuple, group: str = "producer") -> str:
    """The fingerprint of one entry tuple; ValueError when the source lacks an entry name."""
    fp = fingerprints(source, {group: tuple(entries)})[group]
    if fp is None:
        raise ValueError(f"producer fingerprint: the source lacks one of {tuple(entries)}")
    return fp
