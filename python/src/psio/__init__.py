"""psio: PSSZ, key and JSON / I-JSON serialization from one set of annotated type declarations.

    from dataclasses import dataclass
    from typing import Annotated
    import psio
    from psio import u64

    @dataclass
    class Account:
        id: u64
        owner: Annotated[str, psio.max(32)]
        tags: Annotated[list[str], psio.max(8), psio.element(psio.max(16))]
        balance: u64

    data = psio.to_pssz(Account(7, "alice", ["vip", "beta"], 100))
    account = psio.from_pssz(Account, data)          # validates, then decodes
    text = psio.to_ijson(account)
    schema = psio.schema_of(Account)                 # the Schema IR C++ / Rust / JS agree on

See ``python/README.md``.  (Import the names you use: ``from psio import *`` would shadow the builtins
``max``, ``min`` and ``sorted``.)
"""

from . import ir
from .annotations import (  # noqa: F401
    Annotated, DurationMs, DurationNs, DurationSec, DurationUs, RawString, TimePointMs, TimePointNs,
    TimePointSec, TimePointUs, alt, array, at, bitlist, bitvector, bounded, boxed, char, custom, duration, element,
    exact, f32, f64, i8, i16, i32, i64, i128, identity, ijson_number, ijson_string, json_number, json_string,
    keys, length, max, min, monotonic, name, nested, sorted, time_point, u8, u16, u32, u64, u128, u256, unique,
    unique_keys, utf8, values,
)
from .api import (  # noqa: F401
    from_ijson, from_json, from_key, from_pssz, from_pssz_trusted, json_to_pssz, key_prefix, key_size,
    pssz_size, pssz_to_json, pssz_view, to_ijson, to_json, to_key, to_pssz, validate_ijson, validate_json,
    validate_key, validate_pssz,
)
from .evolution import evolves_from  # noqa: F401
from .errors import DecodeError, EncodeError, InternalError, JsonError, PsioError, SchemaError  # noqa: F401
from .interface import Interface, Method, interface, interface_of, method  # noqa: F401
from .ir import IR_VERSION  # noqa: F401
from ._names import is_hash_name, method_to_number, name_to_number, number_to_method, number_to_name  # noqa: F401
from .schema import Handler, Schema, SchemaMethod, SchemaType, Status, key_prefix_upper_bound, trusted  # noqa: F401
from ._time import time_point_from_iso, time_point_to_iso  # noqa: F401
from .typed import arguments, codec, from_text, invariant, record, schema_of, to_text, transparent  # noqa: F401
from .typed import enum_type as enum  # noqa: F401
from .values import NaN, Bits, Some, Variant  # noqa: F401
from .view import ListView, RecordView, TupleView, VariantView, View  # noqa: F401

__version__ = "0.1.0"

__all__ = [
    "ir", "interface", "SchemaMethod", "Annotated", "Bits", "DecodeError", "DurationMs", "DurationNs", "DurationSec", "DurationUs",
    "EncodeError", "Handler", "IR_VERSION", "Interface", "InternalError", "JsonError", "ListView", "Method", "NaN",
    "PsioError", "RawString", "RecordView", "Schema", "SchemaError", "SchemaType", "Some", "Status", "TimePointMs",
    "TimePointNs", "TimePointSec", "TimePointUs", "TupleView", "Variant", "VariantView", "View", "alt",
    "arguments", "array", "at", "bitlist", "bitvector", "bounded", "boxed", "char", "codec", "custom", "duration",
    "element", "enum", "exact", "f32", "f64", "from_ijson", "from_json", "from_key", "from_pssz",
    "from_pssz_trusted", "from_text", "i128", "i16", "i32", "i64", "i8", "identity", "ijson_number",
    "ijson_string", "interface_of", "invariant", "json_number", "json_string", "json_to_pssz", "key_prefix",
    "key_prefix_upper_bound", "key_size", "keys", "length", "max", "method", "min", "monotonic", "name", "nested",
    "pssz_size", "pssz_to_json", "pssz_view", "record", "schema_of", "time_point", "time_point_from_iso",
    "time_point_to_iso", "to_ijson", "to_json", "to_key", "to_pssz", "to_text", "transparent", "trusted", "u128",
    "u16", "u256", "u32", "u64", "u8", "utf8", "validate_ijson", "validate_json", "validate_key", "validate_pssz",
    "sorted", "unique", "unique_keys", "is_hash_name", "method_to_number", "name_to_number", "number_to_method",
    "number_to_name", "evolves_from",
]
