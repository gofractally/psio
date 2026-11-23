"""The Schema IR (``psio::Schema``, cpp/include/psio/schema_ir.hpp) as Python values.

The IR here is exactly the value the PSSZ / I-JSON encoding of ``psio::Schema``
decodes to under this library's dynamic value model: a record is a ``dict``, a
variant is a :class:`~psio.values.Variant`, an optional is ``None`` or the
value.  An ``AnyType`` is therefore ``{"value": Variant(kind, body)}``.

A compact spelling is accepted wherever a type is expected and normalized:

* a ``str`` names a type of the schema;
* ``{"Int": {"bits": 32, "isSigned": False}}``, ``{"List": T}``, ``{"Option": T}``,
  ``{"Nested": T}``, ``{"Tuple": [T, ...]}``, ``{"Array": {"type": T, "len": n}}``,
  ``{"BoundedList": {"type": T, "maxCount": n}}``, ``{"Custom": {"type": T, "id": s}}``,
  ``{"Enum": {"type": T, "members": {name: value}}}``, and
  ``{"Struct" | "Object" | "Variant": {name: T, ...}}`` (or the full
  ``{"members": [...], "attributes": [...]}``).
"""

from __future__ import annotations

from typing import Any, Dict, List, Mapping, Optional

from .errors import SchemaError
from .values import Variant

__all__ = [
    "IR_VERSION", "normalize_type", "normalize_schema", "check_ir_version", "META_SCHEMA",
    "U8", "U16", "U32", "U64", "I8", "I16", "I32", "I64", "U128", "I128", "U256", "F32", "F64",
    "BOOL", "CHAR", "STRING", "RAW_STRING", "BYTES", "int_type", "list_of", "option_of", "nested_of",
    "array_of", "bounded_list_of", "tuple_of", "custom_of", "type_ref", "member", "attribute",
]

#: The version of the IR's meaning this library reads and writes.
IR_VERSION = 1

U64_MAX = (1 << 64) - 1

IR_KINDS = frozenset((
    "Struct", "Object", "Array", "List", "BoundedList", "Option", "Variant", "Tuple", "Int",
    "Float", "Nested", "Custom", "Resource", "Type", "Enum",
))

IrType = Dict[str, Any]
IrSchema = Dict[str, Any]


def _fail(message: str) -> Any:
    raise SchemaError(message)


def _u64(x: Any, what: str) -> int:
    if type(x) is str and x.isascii() and x.isdigit() and (x == "0" or x[0] != "0") and len(x) <= 20:
        x = int(x)
    if type(x) is not int:
        _fail(f"pssz IR: {what} must be an unsigned integer")
    if x < 0 or x > U64_MAX:
        _fail(f"pssz IR: {what} out of range")
    return int(x)


def _enum_value(x: Any) -> int:
    if type(x) is int and x < 0:
        if x < -(1 << 63):
            _fail("pssz IR: Enumerator.value out of range")
        return x & U64_MAX
    return _u64(x, "Enumerator.value")


def _u32(x: Any, what: str) -> int:
    if type(x) is not int or x < 0 or x > 0xFFFFFFFF:
        _fail(f"pssz IR: {what} must be a 32-bit unsigned integer")
    return int(x)


def _str(x: Any, what: str) -> str:
    if type(x) is not str:
        _fail(f"pssz IR: {what} must be a string")
    return str(x)


def _attributes(x: Any) -> List[Dict[str, Any]]:
    if x is None:
        return []
    if not isinstance(x, (list, tuple)):
        _fail("pssz IR: attributes must be an array")
    out = []
    for a in x:
        if not isinstance(a, Mapping):
            _fail("pssz IR: malformed attribute")
        value = a.get("value")
        out.append({"name": _str(a.get("name"), "attribute name"),
                    "value": None if value is None else _str(value, "attribute value")})
    return out


def _member(x: Any, depth: int) -> Dict[str, Any]:
    if not isinstance(x, Mapping):
        _fail("pssz IR: malformed member")
    return {"name": _str(x.get("name"), "member name"), "type": normalize_type(x.get("type"), depth),
            "attributes": _attributes(x.get("attributes"))}


def _record(x: Any, depth: int) -> Dict[str, Any]:
    if not isinstance(x, Mapping):
        _fail("pssz IR: malformed record")
    if isinstance(x.get("members"), (list, tuple)):
        return {"members": [_member(m, depth) for m in x["members"]],
                "attributes": _attributes(x.get("attributes"))}
    return {"members": [{"name": _str(name, "member name"), "type": normalize_type(t, depth), "attributes": []}
                        for name, t in x.items()],
            "attributes": []}


def normalize_type(x: Any, depth: int = 0) -> IrType:
    """Validates and copies a type in either spelling into the canonical IR."""
    if depth > 256:
        _fail("pssz IR: schema nesting too deep")
    if type(x) is str:
        return {"value": Variant("Type", {"type": x})}
    if not isinstance(x, Mapping):
        _fail("pssz IR: malformed type")
    value = x.get("value") if len(x) == 1 else None
    if isinstance(value, Variant):
        kind, body = value.type, value.value
    elif isinstance(value, Mapping) and type(value.get("type")) is str and "value" in value:
        kind, body = value["type"], value["value"]
    else:
        if len(x) != 1:
            _fail("pssz IR: malformed type")
        (kind, body), = x.items()
        if kind in ("List", "Option", "Nested"):
            body = {"type": body}
        elif kind == "Tuple":
            body = {"members": body}
    if kind not in IR_KINDS:
        _fail(f'pssz IR: unknown type kind "{kind}"')
    d = depth + 1
    if kind in ("Struct", "Object", "Variant"):
        return {"value": Variant(kind, _record(body, d))}
    if kind == "Resource":
        return {"value": Variant(kind, body)}
    if not isinstance(body, Mapping):
        _fail("pssz IR: malformed type")
    if kind == "Array":
        return {"value": Variant(kind, {"type": normalize_type(body.get("type"), d),
                                        "len": _u64(body.get("len"), "Array.len")})}
    if kind == "BoundedList":
        return {"value": Variant(kind, {"type": normalize_type(body.get("type"), d),
                                        "maxCount": _u64(body.get("maxCount"), "BoundedList.maxCount")})}
    if kind in ("List", "Option", "Nested"):
        return {"value": Variant(kind, {"type": normalize_type(body.get("type"), d)})}
    if kind == "Tuple":
        members = body.get("members")
        if not isinstance(members, (list, tuple)):
            _fail("pssz IR: malformed tuple")
        return {"value": Variant(kind, {"members": [normalize_type(m, d) for m in members]})}
    if kind == "Int":
        if type(body.get("isSigned")) is not bool:
            _fail("pssz IR: Int.isSigned must be a boolean")
        return {"value": Variant(kind, {"bits": _u32(body.get("bits"), "Int.bits"), "isSigned": body["isSigned"]})}
    if kind == "Float":
        return {"value": Variant(kind, {"exp": _u32(body.get("exp"), "Float.exp"),
                                        "mantissa": _u32(body.get("mantissa"), "Float.mantissa")})}
    if kind == "Custom":
        return {"value": Variant(kind, {"type": normalize_type(body.get("type"), d),
                                        "id": _str(body.get("id"), "Custom.id")})}
    if kind == "Enum":
        raw = body.get("members")
        if isinstance(raw, (list, tuple)):
            members = []
            for m in raw:
                if not isinstance(m, Mapping):
                    _fail("pssz IR: malformed enumerator")
                members.append({"name": _str(m.get("name"), "Enumerator.name"), "value": _enum_value(m.get("value"))})
        elif isinstance(raw, Mapping):
            members = [{"name": _str(name, "Enumerator.name"), "value": _enum_value(v)} for name, v in raw.items()]
        else:
            _fail("pssz IR: malformed enum")
        return {"value": Variant(kind, {"type": normalize_type(body.get("type"), d), "members": members})}
    return {"value": Variant("Type", {"type": _str(body.get("type"), "Type.type")})}


def normalize_schema(x: Any) -> IrSchema:
    """Validates and copies a schema in either spelling into the canonical IR.

    A ``{name: type}`` map is written in the current IR version; the full form
    (``{"types": [...], "ir_version": ...}``) says for itself which it is.
    """
    if not isinstance(x, Mapping):
        _fail("pssz IR: malformed schema")
    version: Optional[int] = IR_VERSION
    package: Dict[str, Any] = {"name": "", "version": "", "attributes": []}
    interfaces: List[Any] = []
    worlds: List[Any] = []
    uses: List[Any] = []
    if isinstance(x.get("types"), (list, tuple)):
        types = [_member(m, 0) for m in x["types"]]
        v = x.get("ir_version")
        if v is None:
            version = None
        elif type(v) is int and 0 <= v <= 0xFFFFFFFF:
            version = v
        else:
            _fail("pssz IR: ir_version must be a 32-bit unsigned integer")
        p = x.get("package")
        if isinstance(p, Mapping):
            package = {"name": _str(p.get("name", ""), "package name"),
                       "version": _str(p.get("version", ""), "package version"),
                       "attributes": _attributes(p.get("attributes"))}
        interfaces = list(x.get("interfaces") or [])
        worlds = list(x.get("worlds") or [])
        uses = list(x.get("uses") or [])
    else:
        types = [{"name": _str(name, "type name"), "type": normalize_type(t), "attributes": []}
                 for name, t in x.items()]
    return {"package": package, "interfaces": interfaces, "worlds": worlds, "uses": uses, "types": types,
            "ir_version": version}


def check_ir_version(schema: IrSchema) -> None:
    v = schema.get("ir_version")
    if v == IR_VERSION:
        return
    if v is None:
        _fail("pssz IR: schema has no ir_version marker (it predates the box / utf8 markers and would "
              "derive different widths); regenerate it")
    _fail("pssz IR: schema ir_version is newer than this library")


# ── Builders (the compact spelling, as values) ──────────────────────────────

def int_type(bits: int, signed: bool) -> IrType:
    return {"Int": {"bits": bits, "isSigned": signed}}


def list_of(t: Any) -> IrType:
    return {"List": t}


def option_of(t: Any) -> IrType:
    return {"Option": t}


def nested_of(t: Any) -> IrType:
    return {"Nested": t}


def array_of(t: Any, n: int) -> IrType:
    return {"Array": {"type": t, "len": n}}


def bounded_list_of(t: Any, n: int) -> IrType:
    return {"BoundedList": {"type": t, "maxCount": n}}


def tuple_of(*ts: Any) -> IrType:
    return {"Tuple": list(ts)}


def custom_of(t: Any, id: str) -> IrType:
    return {"Custom": {"type": t, "id": id}}


def type_ref(name: str) -> IrType:
    return {"value": Variant("Type", {"type": name})}


def attribute(name: str, value: Optional[str] = None) -> Dict[str, Any]:
    return {"name": name, "value": value}


def member(name: str, t: Any, *attributes: Dict[str, Any]) -> Dict[str, Any]:
    return {"name": name, "type": t, "attributes": list(attributes)}


U8, U16, U32, U64 = (int_type(b, False) for b in (8, 16, 32, 64))
I8, I16, I32, I64 = (int_type(b, True) for b in (8, 16, 32, 64))
U128, I128, U256 = int_type(128, False), int_type(128, True), int_type(256, False)
F32: IrType = {"Float": {"exp": 8, "mantissa": 24}}
F64: IrType = {"Float": {"exp": 11, "mantissa": 53}}
BOOL = custom_of(int_type(1, False), "bool")
CHAR = custom_of(U8, "char")
#: UTF-8 text (``str``).
STRING = custom_of(list_of(U8), "utf8")
#: A raw string: any bytes (``bytes``).
RAW_STRING = custom_of(list_of(CHAR), "string")
#: A byte list (``bytes``; hex in text).
BYTES = custom_of(list_of(U8), "hex")


def _object(**members: Any) -> IrType:
    return {"Object": members}


_ATTRIBUTES = list_of("Attribute")
_RECORD = _object(members=list_of("Member"), attributes=_ATTRIBUTES)
_BOXED = _object(type="AnyType")

#: The Schema IR of ``psio::Schema`` itself.
META_SCHEMA: Dict[str, Any] = {
    "Attribute": _object(name=STRING, value=option_of(STRING)),
    "Member": _object(name=STRING, type="AnyType", attributes=_ATTRIBUTES),
    "Func": _object(name=STRING, params=list_of("Member"), result=option_of("AnyType"), attributes=_ATTRIBUTES),
    "Resource": _object(name=STRING, methods=list_of("Func"), attributes=_ATTRIBUTES),
    "AnyType": _object(value={"Variant": {
        "Struct": _RECORD,
        "Object": _RECORD,
        "Array": _object(type="AnyType", len=U64),
        "List": _BOXED,
        "BoundedList": _object(type="AnyType", maxCount=U64),
        "Option": _BOXED,
        "Variant": _RECORD,
        "Tuple": _object(members=list_of("AnyType")),
        "Int": _object(bits=U32, isSigned=BOOL),
        "Float": _object(exp=U32, mantissa=U32),
        "Nested": _BOXED,
        "Custom": _object(type="AnyType", id=STRING),
        "Resource": "Resource",
        "Type": _object(type=STRING),
        "Enum": _object(type="AnyType", members=list_of("Enumerator")),
    }}),
    "Enumerator": _object(name=STRING, value=U64),
    "Package": _object(name=STRING, version=STRING, attributes=_ATTRIBUTES),
    "UseItem": _object(name=STRING, alias=option_of(STRING)),
    "Use": _object(package=STRING, interface_name=STRING, version=STRING, items=list_of("UseItem")),
    "UseRef": _object(package=STRING, interface_name=STRING),
    "Interface": _object(name=STRING, type_names=list_of(STRING), funcs=list_of("Func"), attributes=_ATTRIBUTES),
    "World": _object(name=STRING, imports=list_of("UseRef"), exports=list_of(STRING), attributes=_ATTRIBUTES),
    "Schema": _object(package="Package", interfaces=list_of("Interface"), worlds=list_of("World"),
                      uses=list_of("Use"), types=list_of("Member"), ir_version=option_of(U32)),
}
