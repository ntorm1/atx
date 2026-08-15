#!/usr/bin/env python3
"""Module-wide AST allowlist for the oracle store's sole data projection."""

from __future__ import annotations

import ast
import json
import sys
from pathlib import Path


ALLOWED_COLUMN = "undSecKey_tk"
DATA_READ_ATTRIBUTES = frozenset(
    {"read", "read_row_group", "read_row_groups", "iter_batches", "scan_contents"}
)


def _is_constructor(node: ast.AST) -> bool:
    return (
        isinstance(node, ast.Call)
        and isinstance(node.func, ast.Attribute)
        and isinstance(node.func.value, ast.Name)
        and node.func.value.id == "pq"
        and node.func.attr == "ParquetFile"
    )


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


def _assigned_names(node: ast.AST) -> set[str]:
    if isinstance(node, ast.Name):
        return {node.id}
    if isinstance(node, (ast.Tuple, ast.List)):
        return set().union(*(_assigned_names(item) for item in node.elts))
    return set()


def _root_name(node: ast.AST) -> str | None:
    while isinstance(node, (ast.Attribute, ast.Subscript)):
        node = node.value
    return node.id if isinstance(node, ast.Name) else None


class ModuleInventory(ast.NodeVisitor):
    def __init__(self) -> None:
        self.parents: dict[ast.AST, ast.AST] = {}
        self.bounded_calls: dict[ast.Call, bool] = {}
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
        self.bounded_calls[node] = self._bounded_depth > 0
        self.generic_visit(node)


def _constructor_errors(tree: ast.Module, inventory: ModuleInventory) -> list[str]:
    constructors = [node for node in ast.walk(tree) if _is_constructor(node)]
    if len(constructors) != 1:
        return ["exactly one pq.ParquetFile(...) constructor required"]
    constructor = constructors[0]
    parent = inventory.parents.get(constructor)
    if not (
        isinstance(parent, ast.Assign)
        and parent.value is constructor
        and len(parent.targets) == 1
        and isinstance(parent.targets[0], ast.Name)
        and parent.targets[0].id == "parquet_file"
    ):
        return ["pq.ParquetFile(...) must be directly assigned to parquet_file"]
    return []


def _alias_errors(tree: ast.Module) -> tuple[list[str], set[str]]:
    aliases = {"parquet_file"}
    errors: list[str] = []
    changed = True
    while changed:
        changed = False
        for node in ast.walk(tree):
            if isinstance(node, ast.Assign):
                targets = set().union(*(_assigned_names(target) for target in node.targets))
                value = node.value
            elif isinstance(node, ast.AnnAssign):
                targets = _assigned_names(node.target)
                value = node.value
            elif isinstance(node, ast.NamedExpr):
                targets = _assigned_names(node.target)
                value = node.value
            else:
                continue
            if value is None or not targets:
                continue
            root = _root_name(value)
            object_alias = isinstance(value, ast.Name) and value.id in aliases
            captured_read = (
                isinstance(value, ast.Attribute)
                and root in aliases
                and value.attr in DATA_READ_ATTRIBUTES
            )
            if object_alias or captured_read:
                new_aliases = targets - aliases
                if new_aliases:
                    aliases.update(new_aliases)
                    changed = True
                message = "parquet_file object alias forbidden" if object_alias else "Parquet read method capture forbidden"
                if message not in errors:
                    errors.append(message)
    return errors, aliases


def _getattr_errors(tree: ast.Module, aliases: set[str]) -> list[str]:
    errors: list[str] = []
    for node in ast.walk(tree):
        if not (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id == "getattr"
        ):
            continue
        if node.args and _root_name(node.args[0]) in aliases:
            errors.append("getattr touching parquet_file or its alias forbidden")
        else:
            # This helper needs no dynamic attribute access. Fail closed so a
            # constructor/method alias cannot hide behind a future expression.
            errors.append("getattr forbidden in oracle projection module")
    return errors


def _allowed_projection_errors(
    attribute: ast.Attribute, inventory: ModuleInventory
) -> list[str]:
    errors: list[str] = []
    call = inventory.parents.get(attribute)
    if not (
        attribute.attr == "read_row_group"
        and isinstance(attribute.value, ast.Name)
        and attribute.value.id == "parquet_file"
        and isinstance(call, ast.Call)
        and call.func is attribute
    ):
        return ["non-allowlisted Parquet data-read attribute forbidden"]
    if not inventory.bounded_calls.get(call, False):
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

    parent = inventory.parents.get(call)
    grandparent = inventory.parents.get(parent) if parent is not None else None
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


def _data_read_errors(tree: ast.Module, inventory: ModuleInventory) -> list[str]:
    attributes = [
        node
        for node in ast.walk(tree)
        if isinstance(node, ast.Attribute)
        and isinstance(node.ctx, ast.Load)
        and node.attr in DATA_READ_ATTRIBUTES
    ]
    errors: list[str] = []
    allowed = 0
    for attribute in attributes:
        attribute_errors = _allowed_projection_errors(attribute, inventory)
        if attribute_errors:
            errors.extend(attribute_errors)
        else:
            allowed += 1
    if allowed != 1:
        errors.append("exactly one allowlisted Parquet read_row_group call required")
    return errors


def projection_errors(source: str) -> list[str]:
    try:
        tree = ast.parse(source)
    except SyntaxError as exc:
        return [f"syntax error: {exc.msg}"]
    inventory = ModuleInventory()
    inventory.visit(tree)
    errors = _constructor_errors(tree, inventory)
    alias_errors, aliases = _alias_errors(tree)
    errors.extend(alias_errors)
    errors.extend(_getattr_errors(tree, aliases))
    errors.extend(_data_read_errors(tree, inventory))
    return errors


VALID_FIXTURE = """
parquet_file = pq.ParquetFile(path)
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
parquet_file = pq.ParquetFile(path)
columns = ["undSecKey_tk"]
for row_group_index in range(metadata.num_row_groups):
    parquet_file.read_row_group(row_group_index, columns=columns).column("undSecKey_tk")
""",
    "extra_columns": """
parquet_file = pq.ParquetFile(path)
for row_group_index in range(metadata.num_row_groups):
    parquet_file.read_row_group(
        row_group_index, columns=["undSecKey_tk", "okey_tk"]
    ).column("undSecKey_tk")
""",
}

BYPASS_ATTACKS = {
    "aliased_read": VALID_FIXTURE + "\nalias = parquet_file\nalias.read()\n",
    "aliased_getattr": VALID_FIXTURE + '\nalias = parquet_file\ngetattr(alias, "read")()\n',
    "second_object": VALID_FIXTURE + "\nother = pq.ParquetFile(other_path)\nother.read()\n",
    "temporary_object": VALID_FIXTURE + "\npq.ParquetFile(other_path).read()\n",
    "captured_method": VALID_FIXTURE + "\nreader = parquet_file.read\nreader()\n",
    "dynamic_getattr": VALID_FIXTURE + "\nmethod = 'read'\ngetattr(parquet_file, method)()\n",
}


def _bounded_attack(statement: str) -> str:
    return (
        "parquet_file = pq.ParquetFile(path)\n"
        f"for row_group_index in range(metadata.num_row_groups):\n    {statement}\n"
    )


def main() -> int:
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("atx-vol/scripts/oracle_store_metadata.py")
    production_errors = projection_errors(target.read_text(encoding="utf-8"))
    if production_errors:
        raise SystemExit("production projection rejected: " + "; ".join(production_errors))
    if projection_errors(VALID_FIXTURE):
        raise SystemExit("valid projection fixture rejected")

    attacks = {name: _bounded_attack(source) for name, source in DIRECT_API_ATTACKS.items()}
    attacks.update(SHAPE_ATTACKS)
    attacks.update(BYPASS_ATTACKS)
    false_passes = [name for name, source in attacks.items() if not projection_errors(source)]
    if false_passes:
        raise SystemExit("projection guard false-pass: " + ",".join(false_passes))
    print(
        json.dumps(
            {
                "schema_version": 1,
                "status": "PASS",
                "production_constructors_allowed": 1,
                "production_calls_allowed": 1,
                "direct_api_attacks_rejected": len(DIRECT_API_ATTACKS),
                "shape_attacks_rejected": len(SHAPE_ATTACKS),
                "bypass_attacks_rejected": len(BYPASS_ATTACKS),
            },
            separators=(",", ":"),
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
