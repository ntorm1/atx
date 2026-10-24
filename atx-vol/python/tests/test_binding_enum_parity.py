"""Every bound enum lists every enumerator its C++ declaration has (lane PY, 2026-10-23).

A static source gate, because it must cover enums WITHOUT a C++ ``to_string`` too (those the
runtime reflection in test_python_contract_parity.py cannot see). Each binding TU's
``.value("PY_NAME", Qualified::Enum::Enumerator)`` calls are grouped by their qualified enum
and compared with the ``enum class`` declaration of that name in the public headers
(atx-vol/include and atx-core/include). An enumerator added in C++ and not bound fails
here by name; so does a binding of an enumerator the header no longer declares.

The scan is textual (comments stripped, ``enum class Name [: type] { ... }`` bodies split on
commas). Several nested enums share a bare name (``Kind`` in strategy.hpp); a bound group is
matched to the declaration whose enumerator set equals it, so the gate stays exact there.
Pure Python over the source tree: it imports nothing from atxvol and runs in the driver's
Arrow-safe partition.
"""

from __future__ import annotations

import pathlib
import re

HERE = pathlib.Path(__file__).resolve().parent
VOL = HERE.parents[1]  # atx-vol/
BINDINGS = VOL / "python" / "src" / "bindings"
HEADER_ROOTS = (VOL / "include", VOL.parent / "atx-core" / "include")

_VALUE_RE = re.compile(r'\.value\(\s*"(\w+)"\s*,\s*((?:\w+::)*\w+)::(\w+)\s*\)')
_ENUM_RE = re.compile(r"enum\s+class\s+(\w+)\s*(?::\s*[\w:]+\s*)?\{(.*?)\}", re.S)


def _strip_comments(text: str) -> str:
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", text, flags=re.S))


def _declarations() -> dict[str, list[tuple[str, frozenset[str]]]]:
    out: dict[str, list[tuple[str, frozenset[str]]]] = {}
    for root in HEADER_ROOTS:
        for header in sorted(root.rglob("*.hpp")):
            text = _strip_comments(header.read_text(encoding="utf-8", errors="replace"))
            for name, body in _ENUM_RE.findall(text):
                names = frozenset(m.group(1) for part in body.split(",")
                                  if (m := re.match(r"\s*(\w+)", part)))
                out.setdefault(name, []).append((header.name, names))
    return out


def _bound() -> dict[str, dict[str, set[str]]]:
    out: dict[str, dict[str, set[str]]] = {}
    for tu in sorted(BINDINGS.glob("*.cpp")):
        for _py, qual, enumerator in _VALUE_RE.findall(tu.read_text(encoding="utf-8")):
            out.setdefault(qual, {}).setdefault(tu.name, set()).add(enumerator)
    return out


def test_the_scan_sees_the_binding_tus_and_their_enums():
    bound = _bound()
    # 98 qualified enums at lane PY's close; a collapse to a handful means the regex broke.
    assert len(bound) >= 80, sorted(bound)
    assert "ArchiveStatus" in bound and "ServeStatus" in bound and "AlExerciseClass" in bound


def test_every_bound_enum_lists_every_declared_enumerator():
    declarations = _declarations()
    problems = []
    for qual, per_tu in sorted(_bound().items()):
        enum = qual.split("::")[-1]
        got = set().union(*per_tu.values())
        candidates = declarations.get(enum, [])
        if not candidates:
            problems.append(f"{qual}: no `enum class {enum}` in the public headers")
            continue
        if any(names == got for _, names in candidates):
            continue
        supersets = [(h, names) for h, names in candidates if names >= got]
        if not supersets:
            problems.append(f"{qual} ({', '.join(per_tu)}): binds enumerators no header "
                            f"declares: {sorted(got - set().union(*(n for _, n in candidates)))}")
            continue
        header, names = min(supersets, key=lambda c: len(c[1]))
        problems.append(f"{qual} ({', '.join(per_tu)}): {header} declares "
                        f"{sorted(names - got)} which the binding does not list")
    assert not problems, "\n".join(problems)
