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

See ``python/README.md``.
"""

from . import ir
from .annotations import (  # noqa: F401
    Annotated, DurationMs, DurationNs, DurationSec, DurationUs, RawString, TimePointMs, TimePointNs,
    TimePointSec, TimePointUs, alt, array, at, bitlist, bitvector, bounded, boxed, char, custom, duration, element,
    exact, f32, f64, i8, i16, i32, i64, i128, identity, ijson_number, ijson_string, json_number, json_string,
    keys, length, max, min, monotonic, name, nested, time_point, u8, u16, u32, u64, u128, u256, utf8, values,
)
from .api import (  # noqa: F401
    from_ijson, from_json, from_key, from_pssz, from_pssz_trusted, json_to_pssz, key_prefix, key_size,
    pssz_size, pssz_to_json, pssz_view, to_ijson, to_json, to_key, to_pssz, validate_ijson, validate_json,
    validate_key, validate_pssz,
)
from .errors import DecodeError, EncodeError, InternalError, JsonError, PsioError, SchemaError  # noqa: F401
from .ir import IR_VERSION  # noqa: F401
from .schema import Handler, Schema, SchemaType, Status, key_prefix_upper_bound, trusted  # noqa: F401
from ._time import time_point_from_iso, time_point_to_iso  # noqa: F401
from .typed import arguments, codec, from_text, invariant, record, schema_of, to_text, transparent  # noqa: F401
from .typed import enum_type as enum  # noqa: F401
from .values import NaN, Bits, Some, Variant  # noqa: F401
from .view import ListView, RecordView, TupleView, VariantView, View  # noqa: F401

__version__ = "0.1.0"
