"""Formats as values (gofractally/psio#18).

The package has one function family per format (``psio.to_pssz``, ``psio.to_key``, ``psio.to_json``, ...).
Code that is generic over the format takes one of these values instead and calls the same members whatever
the format::

    from psio import formats

    def store(fmt, value):
        return fmt.encode_checked(value)

    store(formats.pssz, account)        # or formats.key, formats.json, formats.ijson

The members are the operations every format has (C++ ``psio::format``, ``psio/format_concept.hpp``).  What a
format may lack is a capability: ``view`` and ``decode_trusted`` are ``None`` where the format has none.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable, Optional

from . import api

__all__ = ["Format", "pssz", "key", "json", "ijson", "ALL"]


@dataclass(frozen=True)
class Format:
    """One format.  ``encode`` returns ``bytes``, or ``str`` for a text format."""

    #: The format's name, as C++ spells the tag (``psio::pssz`` is ``"pssz"``).
    name: str
    #: The encoding is text (``str``).
    text: bool
    #: The fast write: ``encode(value, T=None)``.
    encode: Callable[..., Any]
    #: The validated write: the value is held against what the type declares.
    encode_checked: Callable[..., Any]
    #: The validating read: ``decode(T, data)``.
    decode: Callable[[Any, Any], Any]
    #: ``validate(T, data)`` -> ``Status``.  Never raises on bad data.
    validate: Callable[[Any, Any], Any]
    #: ``size(value, T=None)``: the size of the encoding.
    size: Callable[..., int]
    #: ``decode_trusted(T, data)``; ``None`` where the format has no trusted read.
    decode_trusted: Optional[Callable[[Any, Any], Any]] = None
    #: ``view(T, data)``: reads in place; ``None`` where the format has no view.
    view: Optional[Callable[[Any, Any], Any]] = None


pssz = Format("pssz", False, api.to_pssz, api.to_pssz_checked, api.from_pssz, api.validate_pssz, api.pssz_size,
              decode_trusted=api.from_pssz_trusted, view=api.pssz_view)
key = Format("key", False, api.to_key, api.to_key_checked, api.from_key, api.validate_key, api.key_size)
json = Format("json", True, api.to_json, api.to_json_checked, api.from_json, api.validate_json,
              lambda value, T=None: len(api.to_json(value, T).encode("utf-8")))
ijson = Format("ijson", True, api.to_ijson, api.to_ijson_checked, api.from_ijson, api.validate_ijson,
               lambda value, T=None: len(api.to_ijson(value, T).encode("utf-8")))

#: Every format of this package.
ALL = (pssz, key, json, ijson)
