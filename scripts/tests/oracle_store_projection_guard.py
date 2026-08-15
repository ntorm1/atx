#!/usr/bin/env python3
"""AST guard for the oracle store's single aggregate-only data projection."""

from __future__ import annotations

import ast
import json
import sys
from pathlib import Path


ALLOWED_COLUMN = "undSecKey_tk"


def _is_bounded_row_group_loop(node: ast.For) -> bool:
    iterator = node.iter
    return (
        isinstance(node.target, ast.Name)
        and node.target.id == "row_group_index"
        and isinstance(iterator, ast.Call)
        and isinstance(iterator.func, ast.Name)
        and iterator.func.id == "range"
        and len(iterator.args) == 1
        and not iterator.keywords
        and isinstance(iterator.args[0], ast.Attribute)
        and isinstance(iterator.args[0].value, ast.Name)
        and iterator.args[0].value.id == "metadata"
        and iterator.args[0].attr == "num_row_groups"
    )


class ProjectionVisitor(ast.NodeVisitor):
    def __init__(self) -> None:
        self.direct_calls: list[tuple[ast.Call, bool]] = []
        self.dynamic_calls: list[ast.Call] = []
        self.parents: dict[ast.AST, ast.AST] = {}
        self._bounded_depth = 0

    def generic_visit(self, node: ast.AST) -> None:
        for child in ast.iter_child_nodes(node):
            self.parents[child] = node
        super().generic_visit(node)

    def visit_For(self, node: ast.For) -> None:
        bounded = _is_bounded_row_group_loop(node)
        if bounded:
            self._bounded_depth += 1
        self.generic_visit(node)
        if bounded:
            self._bounded_depth -= 1

    def visit_Call(self, node: ast.Call) -> None:
        receiver = node.func.value if isinstance(node.func, ast.Attribute) else None
        if isinstance(receiver, ast.Name) and receiver.id == "parquet_file":
            self.direct_calls.append((node, self._bounded_depth > 0))
        if (
            isinstance(node.func, ast.Call)
            and isinstance(node.func.func, ast.Name)
            and node.func.func.id == "getattr"
            and node.func.args
            and isinstance(node.func.args[0], ast.Name)
            and node.func.args[0].id == "parquet_file"
        ):
            self.dynamic_calls.append(node)
        self.generic_visit(node)


def projection_errors(source: str) -> list[str]:
    try:
        tree = ast.parse(source)
    except SyntaxError as exc:
        return [f"syntax error: {exc.msg}"]
    visitor = ProjectionVisitor()
    visitor.visit(tree)
    errors: list[str] = []
    if visitor.dynamic_calls:
        errors.append("dynamic parquet_file method call forbidden")
    if len(visitor.direct_calls) != 1:
        errors.append("exactly one direct parquet_file data call required")
        return errors

    call, inside_bounded_loop = visitor.direct_calls[0]
    method = call.func.attr if isinstance(call.func, ast.Attribute) else ""
    if method != "read_row_group":
        errors.append(f"parquet_file.{method} forbidden")
    if not inside_bounded_loop:
        errors.append("read_row_group must be inside the bounded row-group loop")
    if len(call.args) != 1 or not isinstance(call.args[0], ast.Name) or call.args[0].id != "row_group_index":
        errors.append("read_row_group row-group argument must be row_group_index")
    if len(call.keywords) != 1 or call.keywords[0].arg != "columns":
        errors.append("read_row_group accepts only the columns keyword")
    else:
        columns = call.keywords[0].value
        if not (
            isinstance(columns, ast.List)
            and len(columns.elts) == 1
            and isinstance(columns.elts[0], ast.Constant)
            and columns.elts[0].value == ALLOWED_COLUMN
        ):
            errors.append("columns must be the exact literal ['undSecKey_tk']")

    parent = visitor.parents.get(call)
    grandparent = visitor.parents.get(parent) if parent is not None else None
    if not (
        isinstance(parent, ast.Attribute)
        and parent.attr == "column"
        and isinstance(grandparent, ast.Call)
        and grandparent.func is parent
        and len(grandparent.args) == 1
        and not grandparent.keywords
        and isinstance(grandparent.args[0], ast.Constant)
        and grandparent.args[0].value == ALLOWED_COLUMN
    ):
        errors.append("projection must immediately select only undSecKey_tk")
    return errors


VALID_FIXTURE = """
for row_group_index in range(metadata.num_row_groups):
    projection = parquet_file.read_row_group(
        row_group_index, columns=["undSecKey_tk"]
    ).column("undSecKey_tk")
"""

# These are the exact four alternate Parquet read APIs the allowlist must reject.
DIRECT_API_ATTACKS = {
    "read_all": "parquet_file.read()",
    "read_other_column": 'parquet_file.read(columns=["okey_tk"])',
    "read_row_groups": 'parquet_file.read_row_groups([row_group_index], columns=["undSecKey_tk"])',
    "scan_contents": 'parquet_file.scan_contents(columns=["undSecKey_tk"])',
}

SHAPE_ATTACKS = {
    "additional_read_row_group": VALID_FIXTURE
    + '\nparquet_file.read_row_group(row_group_index, columns=["undSecKey_tk"]).column("undSecKey_tk")',
    "dynamic_columns": """
columns = ["undSecKey_tk"]
for row_group_index in range(metadata.num_row_groups):
    parquet_file.read_row_group(row_group_index, columns=columns).column("undSecKey_tk")
""",
    "extra_columns": """
for row_group_index in range(metadata.num_row_groups):
    parquet_file.read_row_group(
        row_group_index, columns=["undSecKey_tk", "okey_tk"]
    ).column("undSecKey_tk")
""",
}


def _bounded_attack(statement: str) -> str:
    return f"for row_group_index in range(metadata.num_row_groups):\n    {statement}\n"


def main() -> int:
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("atx-vol/scripts/oracle_store_metadata.py")
    production_errors = projection_errors(target.read_text(encoding="utf-8"))
    if production_errors:
        raise SystemExit("production projection rejected: " + "; ".join(production_errors))
    if projection_errors(VALID_FIXTURE):
        raise SystemExit("valid projection fixture rejected")

    attacks = {name: _bounded_attack(source) for name, source in DIRECT_API_ATTACKS.items()}
    attacks.update(SHAPE_ATTACKS)
    false_passes = [name for name, source in attacks.items() if not projection_errors(source)]
    if false_passes:
        raise SystemExit("projection guard false-pass: " + ",".join(false_passes))
    print(
        json.dumps(
            {
                "schema_version": 1,
                "status": "PASS",
                "production_calls_allowed": 1,
                "direct_api_attacks_rejected": len(DIRECT_API_ATTACKS),
                "shape_attacks_rejected": len(SHAPE_ATTACKS),
            },
            separators=(",", ":"),
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
