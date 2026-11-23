"""Exceptions raised by psio.

Every entry point raises a :class:`PsioError` subclass and nothing else for
bad input.  ``str(error)`` is the reason text; where the shared conformance
fixtures pin a reason (``conformance/*.json``), the text is the fixture's.
"""

from __future__ import annotations

from typing import Optional

__all__ = [
    "PsioError", "SchemaError", "EncodeError", "DecodeError", "JsonError", "InternalError",
]


class PsioError(Exception):
    """Base class.  ``kind`` is one of ``schema``, ``value``, ``bytes``, ``json``, ``internal``."""

    kind = "internal"

    def __init__(self, message: str, *, invariant: bool = False, offset: Optional[int] = None) -> None:
        super().__init__(message)
        #: The reason text.
        self.reason = message
        #: The error is a type's own invariant refusing a structurally valid value.
        self.invariant = invariant
        #: Byte offset in the text, for JSON errors.
        self.offset = offset


class SchemaError(PsioError, TypeError):
    """A type declaration or a Schema IR is malformed, or not expressible in the format."""

    kind = "schema"


class EncodeError(PsioError, ValueError):
    """A Python value is not a value of its type (writers)."""

    kind = "value"


class DecodeError(PsioError, ValueError):
    """Binary input (PSSZ or key bytes) was rejected."""

    kind = "bytes"


class JsonError(PsioError, ValueError):
    """JSON / I-JSON text was rejected."""

    kind = "json"


class InternalError(PsioError):
    """A defect in this library (never expected)."""

    kind = "internal"


_KINDS = {"schema": SchemaError, "value": EncodeError, "bytes": DecodeError, "json": JsonError,
          "internal": InternalError}


def error_of(kind: str, message: str, *, invariant: bool = False, offset: Optional[int] = None) -> PsioError:
    return _KINDS[kind](message, invariant=invariant, offset=offset)
