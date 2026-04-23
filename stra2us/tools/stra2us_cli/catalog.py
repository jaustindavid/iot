"""Catalog schema + loader.

See docs/catalog_spec.md for the canonical spec. This module is a
strict pydantic mirror of that spec, plus `coerce_value` which the CLI
uses to parse operator input against a variable's declared type.

Keep the schema tight: the point of having a schema at all is to catch
typos and drift at load time, not to discover them when a write fails
on the wire.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Literal

import yaml
from pydantic import (
    BaseModel,
    ConfigDict,
    Field,
    ValidationError,
    field_validator,
    model_validator,
)


# ----- schema -----

VAR_NAME_RE = re.compile(r"^[a-z][a-z0-9_]*$")
APP_NAME_RE = re.compile(r"^[a-z][a-z0-9_]*$")

Scope = Literal["app", "device"]
VarType = Literal["int", "float", "string", "bool", "enum"]


class Var(BaseModel):
    model_config = ConfigDict(extra="forbid")

    type: VarType
    scope: list[Scope] = Field(min_length=1)
    default: int | float | str | bool | None = None
    default_per_device: bool = False
    range: tuple[int | float, int | float] | None = None
    values: list[str] | None = None
    format: str | None = None
    help: str | None = None
    ops_only: bool = False
    read_cadence: str | None = None
    enforce: bool = False

    @field_validator("scope")
    @classmethod
    def _unique_scope(cls, v: list[Scope]) -> list[Scope]:
        if len(set(v)) != len(v):
            raise ValueError("scope entries must be unique")
        return v

    @model_validator(mode="after")
    def _cross_field_checks(self) -> "Var":
        t = self.type

        # default vs default_per_device mutually exclusive.
        if self.default is not None and self.default_per_device:
            raise ValueError(
                "use either `default` or `default_per_device: true`, not both"
            )

        # `range` is numeric-only.
        if self.range is not None:
            if t not in ("int", "float"):
                raise ValueError(f"`range` is only valid for int/float (got {t})")
            lo, hi = self.range
            if lo > hi:
                raise ValueError(f"`range` lo > hi ({lo} > {hi})")

        # `values` is enum-only, and required for enum.
        if t == "enum":
            if not self.values:
                raise ValueError("`type: enum` requires `values: [...]`")
            if len(set(self.values)) != len(self.values):
                raise ValueError("`values` entries must be unique")
            if self.default is not None and self.default not in self.values:
                raise ValueError(
                    f"default {self.default!r} not in values {self.values}"
                )
        elif self.values is not None:
            raise ValueError(f"`values` is only valid for type: enum (got {t})")

        # Type-match on default.
        if self.default is not None:
            if t == "int" and not isinstance(self.default, int):
                raise ValueError(f"default for int must be int, got {type(self.default).__name__}")
            if t == "float" and not isinstance(self.default, (int, float)):
                raise ValueError(f"default for float must be number, got {type(self.default).__name__}")
            if t == "string" and not isinstance(self.default, str):
                raise ValueError(f"default for string must be string, got {type(self.default).__name__}")
            if t == "bool" and not isinstance(self.default, bool):
                raise ValueError(f"default for bool must be bool, got {type(self.default).__name__}")
            if t == "enum" and not isinstance(self.default, str):
                raise ValueError(f"default for enum must be string, got {type(self.default).__name__}")

        # Range-match on default for numeric types.
        if self.range is not None and isinstance(self.default, (int, float)) \
                and not isinstance(self.default, bool):
            lo, hi = self.range
            if self.default < lo or self.default > hi:
                raise ValueError(
                    f"default {self.default} outside range [{lo}, {hi}]"
                )

        return self


class Catalog(BaseModel):
    model_config = ConfigDict(extra="forbid")

    app: str
    vars: dict[str, Var]
    version: int = 1

    @field_validator("app")
    @classmethod
    def _app_shape(cls, v: str) -> str:
        if not APP_NAME_RE.match(v):
            raise ValueError(
                f"app name {v!r} must match {APP_NAME_RE.pattern}"
            )
        return v

    @field_validator("vars")
    @classmethod
    def _var_names(cls, v: dict[str, Var]) -> dict[str, Var]:
        if not v:
            raise ValueError("`vars` must contain at least one entry")
        for name in v:
            if not VAR_NAME_RE.match(name):
                raise ValueError(
                    f"variable name {name!r} must match {VAR_NAME_RE.pattern}"
                )
        return v


# ----- loading -----

class CatalogError(RuntimeError):
    """Parse / schema-validation failure. Messages include the offending path."""


def load_catalog(path: Path) -> Catalog:
    """Parse a YAML catalog and return the validated model.

    Raises CatalogError on file-not-found, YAML parse errors, or schema
    violations. The error message includes the field path where possible
    so the author can find it without re-reading the spec.
    """
    if not path.is_file():
        raise CatalogError(f"catalog not found: {path}")
    try:
        with path.open("r") as fh:
            doc = yaml.safe_load(fh)
    except yaml.YAMLError as e:
        raise CatalogError(f"{path}: YAML parse error: {e}") from e
    if not isinstance(doc, dict):
        raise CatalogError(f"{path}: top-level must be a mapping")
    try:
        return Catalog.model_validate(doc)
    except ValidationError as e:
        lines = [f"{path}: schema validation failed:"]
        for err in e.errors():
            loc = ".".join(str(p) for p in err["loc"])
            lines.append(f"  {loc}: {err['msg']}")
        raise CatalogError("\n".join(lines)) from e


# ----- value coercion -----

_TRUE = {"true", "1", "yes", "y", "on"}
_FALSE = {"false", "0", "no", "n", "off"}


def coerce_value(var: Var, raw: str, name: str = "<value>") -> object:
    """Parse a raw CLI string against the variable's declared type.

    Returns a native Python value suitable for msgpack encoding:
      int    → int
      float  → float
      string → str
      bool   → bool
      enum   → str (validated against values)

    Raises CatalogError with a human-readable message on type or range
    mismatch. Does **not** consult `enforce:` — that's a server-side
    concern; the CLI always validates locally.
    """
    t = var.type
    if t == "int":
        try:
            v: object = int(raw)
        except ValueError as e:
            raise CatalogError(f"{name}: expected int, got {raw!r}") from e
    elif t == "float":
        try:
            v = float(raw)
        except ValueError as e:
            raise CatalogError(f"{name}: expected float, got {raw!r}") from e
    elif t == "bool":
        s = raw.strip().lower()
        if s in _TRUE:
            v = True
        elif s in _FALSE:
            v = False
        else:
            raise CatalogError(
                f"{name}: expected bool-ish (true/false/1/0/yes/no), got {raw!r}"
            )
    elif t == "enum":
        assert var.values is not None  # schema guarantees
        if raw not in var.values:
            raise CatalogError(
                f"{name}: {raw!r} not in allowed values {var.values}"
            )
        v = raw
    else:  # string
        v = raw

    if var.range is not None and isinstance(v, (int, float)) and not isinstance(v, bool):
        lo, hi = var.range
        if v < lo or v > hi:
            raise CatalogError(
                f"{name}: value {v} outside catalog range [{lo}, {hi}]"
            )
    return v


def kv_path(app: str, key: str, device: str | None) -> str:
    """Build the KV key for a given scope. `device=None` → app scope."""
    if device is None:
        return f"{app}/{key}"
    return f"{app}/{device}/{key}"
