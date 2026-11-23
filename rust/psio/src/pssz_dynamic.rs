//! Checked runtime codec consuming the actual C++ schema IR mirror
//! (C++ `psio/pssz_ir.hpp`). Unsupported IR markers/adapters return errors;
//! no legacy schema fallback.
//!
//! Two width profiles, selected by the format tag:
//!   `Pssz8` / `Pssz16` / `Pssz32`  the explicit profile: one width inherited
//!       by every type.
//!   `Pssz`  the automatic profile (C++ `Codec<0>`, `encode_auto` /
//!       `decode_auto`). Every record, array and list uses its own
//!       type-derived width, computed from the IR exactly as
//!       `auto_pssz_width::<T>()` computes it from the Rust type: an explicit
//!       `maxDynamicData` attribute rules, otherwise the bound inferred from
//!       bounded lists / strings and member `lengthBound` attributes,
//!       otherwise 4. Caps and length bounds are enforced on encode and
//!       decode.
//!
//! An `Enum` is its underlying type on the wire, everywhere: width, layout,
//! validation, encode and decode all go through `Schema::wire`, and any
//! in-range value of the integer is valid whether or not it is a listed
//! enumerator. The enumerator names matter to the `json` submodule only.
//!
//! Custom ids (`schema_ir::wire_custom_id`): `bool`, `char`, `hex`,
//! `bitvector`, `bitlist` as before; `utf8` is a UTF-8 string, validated on
//! every read and write (`Value::String`); `string` is a RAW string, any
//! bytes (`Value::String` when they are UTF-8, `Value::Bytes` otherwise);
//! `box` is its inner type with no inferred size bound, so a record holding
//! a boxed bounded value derives 4-byte offsets unless it has a cap. Any
//! other id is a custom type (`json::CustomHandlers`): on the wire it is
//! exactly its inner type. `unknown` has no codec.
//!
//! `map` / `set` (docs/pssz-spec.md 5.9): `Custom{List{Tuple{[K, V]}}, "map"}`
//! and `Custom{List{K}, "set"}` (`BoundedList` under a maximum count). The
//! entries are in strictly ascending order of the `key` encoding of K
//! (docs/key-spec.md): every read checks it, every write produces it. A
//! dynamic map value is the `Value::Sequence` of its `[key, value]` entries
//! (in any order when handed to an encoder), a set the sequence of its
//! elements. [`to_key`] is the schema-driven key encoder they are ordered by;
//! [`from_key`] / [`validate_key`] read a key back and [`key_prefix`] writes
//! the key of the leading elements of a composite type.
//!
//! `Nested{type}` (docs/pssz-spec.md 5.8) is a byte list holding the PSSZ
//! encoding of a `type` value, always in the automatic profile. Its dynamic
//! value is the inner value itself: a read validates and decodes the bytes
//! as `type` (the inner root one node level below the Nested node) and a
//! write stores the inner value's canonical encoding. A member `lengthBound`
//! on a Nested member counts those bytes. With [`NestedBytes::Opaque`] the
//! codec does not look inside: the value is `Value::Bytes`.
//!
//! Custom types ([`Options::handlers`]): with a registry, every value of a
//! custom node whose handler has a `check` is checked - on reads after its
//! structure is accepted, on writes before any byte is returned, nested
//! values first - and a strict registry makes a custom node with no handler
//! an error. Without one a custom node is its wire type: structure only.
//!
//! `maxDynamicData` under an explicit width (`Pssz8/16/32`): the type is
//! accepted, the one inherited width is kept, and the cap of the top-level
//! type bounds the whole encoding (encode and decode). Nested caps are not
//! looked up. `maxFields` under an explicit width is refused.
//!
//! Attributes read by the codec:
//!   Object  tuple = "named" | "positional"   an argument list: a record on
//!                                      the wire; in text an object keyed by
//!                                      the member names (a reader also takes
//!                                      the positional array) / the array
//!                                      (`json` submodule)
//!           maxFields = "N"            at most N members, N <= 255
//!           definitionWillNotChange    fixed layout (no header)
//!           maxDynamicData = "N"       explicit cap (automatic profile)
//!   Member  lengthBound = "max=N" | "exact=N" | "min=N" (comma-separated)
//!           jsonInt / ijsonInt = "number" | "string"   (`json` submodule)
//!   Variant  untagged                  text: the bare value of the
//!                                      alternative (`json` submodule)
//!   Variant member  ijsonName = "..."                   (`json` submodule)
//! The JSON / I-JSON text path over the same IR is the `json` submodule.
use crate::pssz::{Decoded, Pssz, PsszError, PsszFormat, PsszResult};
use crate::pssz_checked::{self as checked, Layout};
use crate::pssz_schema_ir::{
    assoc_id, byte_adapter_id, wire_custom_id, AnyType, AnyValue, Attribute, Custom, Member, Schema,
};
use serde::{Deserialize, Serialize};
use std::cell::{Cell, RefCell};
use std::collections::{HashMap, HashSet};

mod compare;
pub mod json;
mod key;
pub use compare::{
    argument_list, equivalent, equivalent_func, equivalent_interface, equivalent_schema,
    evolves_from, evolves_from_func, evolves_from_interface, evolves_from_schema, Compare,
    Compatibility,
};
pub use key::{
    from_key, from_key_with, key_prefix, key_prefix_with, validate_key, validate_key_with,
};
pub use json::{
    json_to_pssz, json_to_pssz_with, json_to_value, json_to_value_with, pssz_to_json,
    pssz_to_json_with, read_json, read_json_with, validate_pssz, validate_pssz_with,
    value_to_json, value_to_json_with, CustomHandlers, SchemaJsonError,
};

/// What the codec does with the bytes of a `Nested` node.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum NestedBytes {
    /// The inner value: validated, decoded and re-encoded as its type.
    #[default]
    Typed,
    /// `Value::Bytes`, not looked at (the rule of a typed deferred nested
    /// value).
    Opaque,
}

/// What a binary entry point is given besides the schema: the registry of
/// custom handlers (their `check`s run, and a strict registry requires a
/// handler for every custom node) and how `Nested` bytes are treated.
#[derive(Clone, Copy, Default)]
pub struct Options<'a> {
    pub handlers: Option<&'a CustomHandlers>,
    pub nested: NestedBytes,
}
impl<'a> Options<'a> {
    pub fn handlers(handlers: &'a CustomHandlers) -> Self {
        Self { handlers: Some(handlers), nested: NestedBytes::Typed }
    }
    pub fn opaque_nested() -> Self {
        Self { handlers: None, nested: NestedBytes::Opaque }
    }
}

pub(crate) const NO_HANDLER: &str = "pssz dynamic: no handler registered for a custom type";
pub(crate) const MAP_DUPLICATE: &str = "pssz dynamic: map has duplicate keys";
pub(crate) const SET_DUPLICATE: &str = "pssz dynamic: set has duplicate elements";
const MAP_ORDER: &str = "pssz: map entries are not in strictly ascending key order";
const SET_ORDER: &str = "pssz: set elements are not in strictly ascending order";
const MAP_SHAPE: &str = "pssz dynamic: a map must sit directly on a list of two-member tuples";
const SET_SHAPE: &str = "pssz dynamic: a set must sit directly on a list";
const NO_KEY: &str = "pssz dynamic: the key type of a map / set has no key encoding";

/// A `map` / `set` Custom node: its list, the list's element, and the key
/// (and mapped value) types.
pub(crate) struct AssocShape<'a> {
    pub map: bool,
    pub list: &'a AnyType,
    /// The element type as written (its custom nodes are checked).
    pub element: &'a AnyType,
    pub key: &'a AnyType,
    pub value: Option<&'a AnyType>,
    /// BoundedList: the maximum number of entries.
    pub max: Option<u64>,
}
pub(crate) fn assoc_shape<'a>(schema: &'a Schema, custom: &'a Custom) -> PsszResult<AssocShape<'a>> {
    let map = custom.id == "map";
    let list = schema.direct(&custom.r#type)?;
    let (element, max): (&AnyType, _) = match &list.value {
        AnyValue::List(l) => (&l.r#type, None),
        AnyValue::BoundedList(l) => (&l.r#type, Some(l.maxCount)),
        _ => return Err(failure(if map { MAP_SHAPE } else { SET_SHAPE })),
    };
    if !map {
        return Ok(AssocShape { map, list, element, key: element, value: None, max });
    }
    match &schema.resolve(element)?.value {
        AnyValue::Tuple(t) if t.members.len() == 2 => Ok(AssocShape {
            map,
            list,
            element,
            key: &t.members[0],
            value: Some(&t.members[1]),
            max,
        }),
        _ => Err(failure(MAP_SHAPE)),
    }
}
/// Whether the type has a `key` encoding (docs/key-spec.md): what the key
/// type of a map and the element type of a set must have.
fn has_key(schema: &Schema, input: &AnyType, active: &mut Vec<usize>) -> PsszResult<()> {
    let (ty, boxed) = schema.wire_boxed(input)?;
    if boxed {
        return Err(failure(NO_KEY));
    }
    let key = ty as *const AnyType as usize;
    if active.contains(&key) {
        return Ok(());
    }
    depth(active.len())?;
    active.push(key);
    let result = (|| match &ty.value {
        AnyValue::Int(i) => {
            int_size(i)?;
            if i.bits == 1 { Err(failure(NO_KEY)) } else { Ok(()) }
        }
        AnyValue::Float(f) => float_size(f).map(|_| ()),
        AnyValue::Option(o) => has_key(schema, &o.r#type, active),
        AnyValue::List(l) => has_key(schema, &l.r#type, active),
        AnyValue::BoundedList(l) => has_key(schema, &l.r#type, active),
        AnyValue::Array(a) => has_key(schema, &a.r#type, active),
        AnyValue::Tuple(t) => t.members.iter().try_for_each(|m| has_key(schema, m, active)),
        AnyValue::Struct(s) => s.members.iter().try_for_each(|m| has_key(schema, &m.r#type, active)),
        AnyValue::Object(o) => o.members.iter().try_for_each(|m| has_key(schema, &m.r#type, active)),
        AnyValue::Variant(v) => v.members.iter().try_for_each(|m| has_key(schema, &m.r#type, active)),
        AnyValue::Custom(c) if matches!(c.id.as_str(), "bool" | "char" | "bitvector") => Ok(()),
        AnyValue::Custom(c) if byte_adapter_id(&c.id) => Ok(()),
        AnyValue::Custom(c) if assoc_id(&c.id) => match assoc_shape(schema, c)?.value {
            Some(value) => has_key(schema, value, active),
            None => Ok(()),
        },
        // The key of a byte list, whatever the inner type is.
        AnyValue::Nested(_) => Ok(()),
        _ => Err(failure(NO_KEY)),
    })();
    active.pop();
    result
}
/// Whether a `Nested` node occurs in (a part of) a key type.
fn key_has_nested(schema: &Schema, input: &AnyType, active: &mut Vec<usize>) -> PsszResult<bool> {
    let ty = schema.wire(input)?;
    let key = ty as *const AnyType as usize;
    if active.contains(&key) {
        return Ok(false);
    }
    depth(active.len())?;
    active.push(key);
    let any = |types: &mut dyn Iterator<Item = &AnyType>, active: &mut Vec<usize>| {
        for ty in types {
            if key_has_nested(schema, ty, active)? {
                return Ok(true);
            }
        }
        Ok(false)
    };
    let result: PsszResult<bool> = match &ty.value {
        AnyValue::Nested(_) => Ok(true),
        AnyValue::Option(o) => any(&mut std::iter::once(&*o.r#type), active),
        AnyValue::List(l) => any(&mut std::iter::once(&*l.r#type), active),
        AnyValue::BoundedList(l) => any(&mut std::iter::once(&*l.r#type), active),
        AnyValue::Array(a) => any(&mut std::iter::once(&*a.r#type), active),
        AnyValue::Custom(c) => any(&mut std::iter::once(&*c.r#type), active),
        AnyValue::Tuple(t) => any(&mut t.members.iter().map(|m| &**m), active),
        AnyValue::Struct(s) => any(&mut s.members.iter().map(|m| &*m.r#type), active),
        AnyValue::Object(o) => any(&mut o.members.iter().map(|m| &*m.r#type), active),
        AnyValue::Variant(v) => any(&mut v.members.iter().map(|m| &*m.r#type), active),
        _ => Ok(false),
    };
    active.pop();
    result
}
fn key_escaped(bytes: &[u8], out: &mut Vec<u8>) {
    for &byte in bytes {
        out.push(byte);
        if byte == 0 {
            out.push(1);
        }
    }
    out.extend_from_slice(&[0, 0]);
}
/// Whether a member type is a `Nested`, looking through Option: its
/// `lengthBound` then counts the bytes of the inner encoding, which only the
/// wire form has (the dynamic value is the inner value).
pub(crate) fn nested_member(schema: &Schema, ty: &AnyType) -> PsszResult<bool> {
    let mut at = schema.wire(ty)?;
    for _ in 0..=64 {
        match &at.value {
            AnyValue::Option(o) => at = schema.wire(&o.r#type)?,
            AnyValue::Nested(_) => return Ok(true),
            _ => return Ok(false),
        }
    }
    Ok(false)
}

/// Typed values preserve distinctions lost by plain JSON, especially
/// None / Some(None), Some(unit), integer widths, and variant selectors.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub enum Value {
    Bool(bool),
    Signed(i128),
    Unsigned(u128),
    Float32(f32),
    Float64(f64),
    String(String),
    Bytes(Vec<u8>),
    Bits { bytes: Vec<u8>, bit_count: u64 },
    Sequence(Vec<Value>),
    Record(Vec<(String, Value)>),
    Optional(Option<Box<Value>>),
    Variant { selector: usize, value: Box<Value> },
}

fn failure(message: &'static str) -> PsszError {
    PsszError::new(message)
}
fn depth(n: usize) -> PsszResult<()> {
    if n > 64 {
        Err(failure("pssz dynamic: depth limit exceeded"))
    } else {
        Ok(())
    }
}
fn dwnc(attributes: &[Attribute]) -> bool {
    attributes
        .iter()
        .any(|a| a.name == "definitionWillNotChange")
}
fn find_attribute<'a>(values: &'a [Attribute], name: &str) -> Option<&'a Attribute> {
    values.iter().find(|a| a.name == name)
}

/// A member's `lengthBound` annotation: bytes of a string, elements of a list.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct LengthBound {
    pub exact: Option<u64>,
    pub max: Option<u64>,
    pub min: Option<u64>,
}
impl LengthBound {
    /// The count that bounds the encoded size: exact, else max.
    fn count(&self) -> Option<u64> {
        self.exact.or(self.max)
    }
    pub(crate) fn admits(&self, n: u64) -> bool {
        !(self.max.is_some_and(|max| n > max)
            || self.exact.is_some_and(|exact| n != exact)
            || self.min.is_some_and(|min| n < min))
    }
    /// The length this bound constrains, looking through Option.
    pub(crate) fn admits_value(&self, value: &Value) -> bool {
        match value {
            Value::Optional(None) => true,
            Value::Optional(Some(inner)) => self.admits_value(inner),
            Value::String(text) => self.admits(text.len() as u64),
            Value::Bytes(bytes) => self.admits(bytes.len() as u64),
            Value::Sequence(items) => self.admits(items.len() as u64),
            _ => true,
        }
    }
}
/// A canonical decimal `u64`: no sign, no leading zero.
fn parse_u64(text: &str) -> Option<u64> {
    let bytes = text.as_bytes();
    if bytes.is_empty()
        || bytes.len() > 20
        || (bytes.len() > 1 && bytes[0] == b'0')
        || !bytes.iter().all(u8::is_ascii_digit)
    {
        return None;
    }
    text.parse().ok()
}
/// "max=N", "exact=N", "min=N", comma-separated, each at most once.
pub(crate) fn parse_length_bound(values: &[Attribute]) -> PsszResult<Option<LengthBound>> {
    const MALFORMED: PsszError = PsszError::new("pssz dynamic: malformed lengthBound attribute");
    let Some(attribute) = find_attribute(values, "lengthBound") else {
        return Ok(None);
    };
    let text = attribute.value.as_deref().filter(|v| !v.is_empty()).ok_or(MALFORMED)?;
    let mut bound = LengthBound::default();
    for item in text.split(',') {
        let (key, number) = item.split_once('=').ok_or(MALFORMED)?;
        let n = parse_u64(number).ok_or(MALFORMED)?;
        let slot = match key {
            "max" => &mut bound.max,
            "exact" => &mut bound.exact,
            "min" => &mut bound.min,
            _ => return Err(MALFORMED),
        };
        if slot.is_some() {
            return Err(MALFORMED);
        }
        *slot = Some(n);
    }
    Ok(Some(bound))
}
fn parse_cap(values: &[Attribute]) -> PsszResult<Option<u64>> {
    let Some(attribute) = find_attribute(values, "maxDynamicData") else {
        return Ok(None);
    };
    attribute
        .value
        .as_deref()
        .and_then(parse_u64)
        .map(Some)
        .ok_or(failure("pssz dynamic: malformed maxDynamicData attribute"))
}
/// `maxFields = "N"`: the most fields the record (or parameters the
/// function) will ever have; N is at most 255 (docs/pssz-spec.md 8.4, 16).
fn parse_max_fields(values: &[Attribute]) -> PsszResult<Option<u64>> {
    let Some(attribute) = find_attribute(values, "maxFields") else {
        return Ok(None);
    };
    attribute
        .value
        .as_deref()
        .and_then(parse_u64)
        .filter(|n| *n <= MAX_RECORD_FIELDS as u64)
        .map(Some)
        .ok_or(failure("pssz dynamic: malformed maxFields attribute"))
}
/// The text form of a record node (docs/json.md "Method arguments"): the
/// `tuple` attribute of the Object of an argument list.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum TupleText {
    /// A record: an object.
    None,
    /// An object keyed by the member names; a reader also takes the array.
    Named,
    /// The array.
    Positional,
}
pub(crate) fn tuple_text(values: &[Attribute]) -> TupleText {
    match find_attribute(values, "tuple").and_then(|a| a.value.as_deref()) {
        None => TupleText::None,
        Some("positional") => TupleText::Positional,
        Some(_) => TupleText::Named,
    }
}
#[derive(Clone, Copy, PartialEq, Eq)]
enum Site {
    /// Object / Struct
    Record,
    RecordMember,
    Variant,
    VariantMember,
}
/// What a profile says about a constraint it does not enforce (`maxFields`
/// under an explicit width).
pub const UNSUPPORTED_CONSTRAINT: &str =
    "pssz dynamic: unsupported schema constraint or annotation";
/// `caps`: the automatic profile, which derives widths from maxDynamicData
/// and enforces every record's. The explicit profile accepts the attribute
/// and enforces the top-level type's only, like the typed `Pssz8/16/32`.
fn attributes(values: &[Attribute], site: Site, caps: bool) -> PsszResult<()> {
    for value in values {
        match value.name.as_str() {
            "doc" | "description" | "visible" => (),
            "definitionWillNotChange" if site == Site::Record => (),
            // The explicit profile takes a cap too: it enforces the
            // top-level type's (`Codec::check_top_cap`).
            "maxDynamicData" if site == Site::Record => (),
            "maxFields" if site == Site::Record && caps => (),
            // An argument list: the same bytes as any record; its text form
            // is the JSON codec's business.
            "tuple" if site == Site::Record => {
                if !matches!(value.value.as_deref(), Some("named" | "positional")) {
                    return Err(failure("pssz dynamic: malformed tuple attribute"));
                }
            }
            "lengthBound" if site == Site::RecordMember => (),
            "jsonInt" | "ijsonInt" if site == Site::RecordMember => {
                if !matches!(value.value.as_deref(), Some("number" | "string")) {
                    return Err(failure("pssz dynamic: malformed integer form attribute"));
                }
            }
            "ijsonName" if site == Site::VariantMember && value.value.is_some() => (),
            // An untagged variant: the same bytes as any variant; in text
            // the bare value of the alternative (`json` submodule).
            "untagged" if site == Site::Variant => (),
            _ => return Err(failure(UNSUPPORTED_CONSTRAINT)),
        }
    }
    if site == Site::RecordMember {
        parse_length_bound(values)?;
    }
    if site == Site::Record {
        parse_cap(values)?;
    }
    Ok(())
}
/// The most elements a tuple (a pair, a method's argument list) can have
/// (docs/pssz-spec.md 8.4; C++ `psio::max_tuple_elements`).
pub const MAX_TUPLE_ELEMENTS: usize = 255;
/// The most fields a record can have: a record, a tuple and an argument
/// list are one positional shape and share one limit (docs/pssz-spec.md
/// 8.4; C++ `psio::max_record_fields`). `maxFields` lowers it for one type.
pub const MAX_RECORD_FIELDS: usize = 255;

/// The field count of a record node against the limit and its `maxFields`.
fn record_fields(members: usize, attributes: &[Attribute]) -> PsszResult<()> {
    if members > MAX_RECORD_FIELDS {
        return Err(failure("pssz dynamic: a record has more than 255 fields"));
    }
    if parse_max_fields(attributes)?.is_some_and(|max| members as u64 > max) {
        return Err(failure("pssz dynamic: a record has more fields than its maxFields"));
    }
    Ok(())
}

/// Checks a whole schema, not only what one root reaches (C++
/// `psio::pssz_ir::validate_schema`): every named type, and every function
/// of every interface and resource — its parameter list is a tuple (at most
/// 255 parameters, docs/pssz-spec.md 8.4; no more than its `maxFields`), its
/// parameter and result types are well-formed, and its `maxDynamicData` /
/// `maxFields` attributes parse. A consumer that takes a schema from outside
/// calls this once before it dispatches through it.
pub fn validate_schema(schema: &Schema) -> PsszResult<()> {
    schema.check_ir_version()?;
    let function = |f: &crate::pssz_schema_ir::Func| -> PsszResult<()> {
        if f.params.len() > MAX_TUPLE_ELEMENTS {
            return Err(failure("pssz dynamic: a function has more than 255 parameters"));
        }
        if parse_max_fields(&f.attributes)?.is_some_and(|max| f.params.len() as u64 > max) {
            return Err(failure("pssz dynamic: a function has more parameters than its maxFields"));
        }
        parse_cap(&f.attributes)?;
        for param in &f.params {
            attributes(&param.attributes, Site::RecordMember, true)?;
            metadata(schema, &param.r#type, true)?;
        }
        if let Some(result) = &f.result {
            metadata(schema, result, true)?;
        }
        Ok(())
    };
    for named in &schema.types {
        if let AnyValue::Resource(resource) = &named.r#type.value {
            for method in &resource.methods {
                function(method)?;
            }
            continue;
        }
        metadata(schema, &named.r#type, true)?;
    }
    for interface in &schema.interfaces {
        for f in &interface.funcs {
            function(f)?;
        }
    }
    Ok(())
}

/// Checks every type reachable from `root`. `caps` is the automatic profile.
pub(crate) fn metadata(schema: &Schema, root: &AnyType, caps: bool) -> PsszResult<()> {
    schema.check_ir_version()?;
    let mut pending = vec![root];
    let mut seen = HashSet::new();
    while let Some(ty) = pending.pop() {
        let ty = schema.wire(ty)?;
        if !seen.insert(ty as *const AnyType as usize) {
            continue;
        }
        let fields = match &ty.value {
            AnyValue::Object(o) => {
                attributes(&o.attributes, Site::Record, caps)?;
                record_fields(o.members.len(), &o.attributes)?;
                Some((o.members.as_slice(), Site::RecordMember))
            }
            AnyValue::Struct(s) => {
                attributes(&s.attributes, Site::Record, caps)?;
                record_fields(s.members.len(), &s.attributes)?;
                Some((s.members.as_slice(), Site::RecordMember))
            }
            AnyValue::Variant(v) => {
                attributes(&v.attributes, Site::Variant, caps)?;
                if v.members.is_empty() || v.members.len() > 256 {
                    return Err(failure("pssz dynamic: invalid variant schema"));
                }
                Some((v.members.as_slice(), Site::VariantMember))
            }
            AnyValue::Tuple(t) => {
                // docs/pssz-spec.md 8.4.
                if t.members.len() > MAX_TUPLE_ELEMENTS {
                    return Err(failure("pssz dynamic: a tuple has more than 255 elements"));
                }
                pending.extend(t.members.iter().map(|t| t.as_ref()));
                None
            }
            AnyValue::List(l) => {
                pending.push(&l.r#type);
                None
            }
            AnyValue::BoundedList(l) => {
                pending.push(&l.r#type);
                None
            }
            AnyValue::Array(a) => {
                pending.push(&a.r#type);
                None
            }
            AnyValue::Option(o) => {
                pending.push(&o.r#type);
                None
            }
            AnyValue::Custom(c) => {
                match c.id.as_str() {
                    "bool" => unsigned_adapter(schema, &c.r#type, 1)?,
                    "char" => unsigned_adapter(schema, &c.r#type, 8)?,
                    "string" | "utf8" | "hex" => {
                        byte_container(schema, &c.r#type)?;
                    }
                    "bitvector" | "bitlist" => {
                        bit_shape(schema, c)?;
                    }
                    "map" | "set" => {
                        let shape = assoc_shape(schema, c)?;
                        has_key(schema, shape.key, &mut Vec::new())?;
                    }
                    _ => return Err(failure("pssz dynamic: unsupported custom adapter")),
                }
                pending.push(&c.r#type);
                None
            }
            AnyValue::Nested(nested) => {
                // The inner type is checked under the automatic profile's
                // rules, which is the profile of its bytes.
                if caps {
                    pending.push(&nested.r#type);
                } else {
                    metadata(schema, &nested.r#type, true)?;
                }
                None
            }
            AnyValue::Int(i) => {
                int_size(i)?;
                None
            }
            AnyValue::Float(f) if matches!((f.exp, f.mantissa), (8, 23 | 24) | (11, 52 | 53)) => {
                None
            }
            _ => return Err(failure("pssz dynamic: unsupported schema type")),
        };
        if let Some((fields, site)) = fields {
            let mut names = HashSet::new();
            for field in fields {
                if !names.insert(field.name.as_str()) {
                    return Err(failure("pssz dynamic: duplicate schema member"));
                }
                attributes(&field.attributes, site, caps)?;
                pending.push(&field.r#type);
            }
        }
    }
    Ok(())
}
fn int_size(int: &crate::pssz_schema_ir::Int) -> PsszResult<usize> {
    match int.bits {
        1 => Ok(1),
        8 | 16 | 32 | 64 | 128 => Ok(int.bits as usize / 8),
        // An unsigned 256-bit integer is carried as its 32 little-endian
        // bytes (`Value::Bytes`).
        256 if !int.isSigned => Ok(32),
        _ => Err(failure("pssz dynamic: unsupported integer width")),
    }
}
fn float_size(float: &crate::pssz_schema_ir::Float) -> PsszResult<usize> {
    match (float.exp, float.mantissa) {
        (8, 23 | 24) => Ok(4),
        (11, 52 | 53) => Ok(8),
        _ => Err(failure("pssz dynamic: unsupported IEEE width")),
    }
}
fn unsigned_adapter(schema: &Schema, ty: &AnyType, bits: u32) -> PsszResult<()> {
    if matches!(schema.direct(ty)?.value, AnyValue::Int(ref i) if i.bits == bits && !i.isSigned) {
        Ok(())
    } else {
        Err(failure(
            "pssz dynamic: primitive adapter has incompatible inner schema",
        ))
    }
}
fn bit_shape(schema: &Schema, custom: &crate::pssz_schema_ir::Custom) -> PsszResult<(usize, bool)> {
    let (element, count, list) = match (&custom.id[..], &schema.direct(&custom.r#type)?.value) {
        ("bitvector", AnyValue::Array(a)) => (&a.r#type, a.len, false),
        ("bitlist", AnyValue::BoundedList(l)) => (&l.r#type, l.maxCount, true),
        _ => {
            return Err(failure(
                "pssz dynamic: packed bit adapter requires bit count metadata",
            ))
        }
    };
    unsigned_adapter(schema, element, 1)?;
    Ok((
        usize::try_from(count)
            .map_err(|_| failure("pssz dynamic: bit count exceeds platform capacity"))?,
        list,
    ))
}

/// How a value of `ty` sits in a record's fixed region. The same under every
/// profile: no width enters it.
pub fn layout<F: PsszFormat>(schema: &Schema, ty: &AnyType) -> PsszResult<Layout> {
    schema.check_ir_version()?;
    layout_at(schema, ty, 0)
}
fn layout_of(schema: &Schema, ty: &AnyType) -> PsszResult<Layout> {
    layout_at(schema, ty, 0)
}
fn layout_at(schema: &Schema, ty: &AnyType, n: usize) -> PsszResult<Layout> {
    depth(n)?;
    let ty = schema.wire(ty)?;
    let mut out = Layout {
        fixed: false,
        size: 0,
        optional: false,
        selector: false,
        omit: false,
    };
    match &ty.value {
        AnyValue::Int(i) => {
            out.fixed = true;
            out.size = int_size(i)?;
        }
        AnyValue::Float(f) => {
            out.fixed = true;
            out.size = float_size(f)?;
        }
        AnyValue::Option(o) => {
            out.optional = true;
            // Selector form iff the payload can encode to zero bytes; the
            // shared frame parser needs it to recognize a stored None.
            out.selector = can_be_empty(schema, &o.r#type, n + 1)?;
        }
        AnyValue::Array(a) => {
            let element = layout_at(schema, &a.r#type, n + 1)?;
            out.fixed = element.fixed;
            if out.fixed {
                out.size = usize::try_from(a.len)
                    .ok()
                    .and_then(|count| count.checked_mul(element.size))
                    .ok_or(failure("pssz dynamic: array size overflow"))?;
            }
        }
        AnyValue::Struct(s) => return record_layout(schema, &s.members, true, n),
        AnyValue::Object(o) if dwnc(&o.attributes) => {
            return record_layout(schema, &o.members, true, n)
        }
        AnyValue::Object(_)
        | AnyValue::Tuple(_)
        | AnyValue::List(_)
        | AnyValue::BoundedList(_)
        | AnyValue::Variant(_) => (),
        AnyValue::Custom(c) if c.id == "bool" => {
            unsigned_adapter(schema, &c.r#type, 1)?;
            out.fixed = true;
            out.size = 1;
        }
        AnyValue::Custom(c) if c.id == "char" => {
            unsigned_adapter(schema, &c.r#type, 8)?;
            out.fixed = true;
            out.size = 1;
        }
        AnyValue::Custom(c) if c.id == "bitvector" || c.id == "bitlist" => {
            let (count, list) = bit_shape(schema, c)?;
            out.fixed = !list;
            if !list {
                out.size = crate::pssz_bits::byte_count(count);
            }
        }
        AnyValue::Custom(c) if byte_adapter_id(&c.id) => {
            return layout_at(schema, &c.r#type, n + 1)
        }
        // A list: variable size.
        AnyValue::Custom(c) if assoc_id(&c.id) => {
            assoc_shape(schema, c)?;
        }
        AnyValue::Custom(_) => return Err(failure("pssz dynamic: unsupported custom adapter")),
        // A byte list: variable size.
        AnyValue::Nested(_) => (),
        AnyValue::Resource(_) => {
            return Err(failure(
                "pssz dynamic: resource values require an explicit adapter",
            ))
        }
        AnyValue::Type(_) | AnyValue::Enum(_) => unreachable!("resolved to the wire type"),
    }
    Ok(out)
}
fn record_layout(
    schema: &Schema,
    fields: &[Member],
    stable: bool,
    n: usize,
) -> PsszResult<Layout> {
    let mut out = Layout {
        fixed: stable,
        size: 0,
        optional: false,
        selector: false,
        omit: false,
    };
    for field in fields {
        let inner = layout_at(schema, &field.r#type, n + 1)?;
        out.fixed &= inner.fixed;
        out.size = out
            .size
            .checked_add(inner.size)
            .ok_or(failure("pssz dynamic: record size overflow"))?;
    }
    if !out.fixed {
        out.size = 0;
    }
    Ok(out)
}
fn can_be_empty(schema: &Schema, ty: &AnyType, n: usize) -> PsszResult<bool> {
    depth(n)?;
    let ty = schema.wire(ty)?;
    Ok(match &ty.value {
        AnyValue::List(_) | AnyValue::BoundedList(_) | AnyValue::Nested(_) => true,
        AnyValue::Custom(c) if assoc_id(&c.id) => true,
        AnyValue::Option(o) => !can_be_empty(schema, &o.r#type, n + 1)?,
        AnyValue::Array(a) => {
            a.len == 0 || {
                let l = layout_at(schema, &a.r#type, n + 1)?;
                l.fixed && l.size == 0
            }
        }
        AnyValue::Struct(s) => {
            let layout = record_layout(schema, &s.members, true, n)?;
            layout.fixed && layout.size == 0
        }
        AnyValue::Object(o) if dwnc(&o.attributes) => {
            let layout = record_layout(schema, &o.members, true, n)?;
            layout.fixed && layout.size == 0
        }
        AnyValue::Custom(c) if byte_adapter_id(&c.id) => {
            can_be_empty(schema, &c.r#type, n + 1)?
        }
        AnyValue::Int(_)
        | AnyValue::Float(_)
        | AnyValue::Object(_)
        | AnyValue::Tuple(_)
        | AnyValue::Variant(_) => false,
        AnyValue::Custom(c) if c.id == "bool" || c.id == "char" => {
            unsigned_adapter(schema, &c.r#type, if c.id == "bool" { 1 } else { 8 })?;
            false
        }
        AnyValue::Custom(c) if c.id == "bitvector" || c.id == "bitlist" => {
            let (count, list) = bit_shape(schema, c)?;
            !list && count == 0
        }
        _ => return Err(failure("pssz dynamic: unsupported optional inner schema")),
    })
}
fn byte_container(schema: &Schema, ty: &AnyType) -> PsszResult<(Option<usize>, bool)> {
    let (element, bound, exact) = match &schema.direct(ty)?.value {
        AnyValue::List(l) => (&l.r#type, None, false),
        AnyValue::BoundedList(l) => (
            &l.r#type,
            Some(usize::try_from(l.maxCount).map_err(|_| failure("pssz dynamic: bound overflow"))?),
            false,
        ),
        AnyValue::Array(a) => (
            &a.r#type,
            Some(usize::try_from(a.len).map_err(|_| failure("pssz dynamic: bound overflow"))?),
            true,
        ),
        _ => {
            return Err(failure(
                "pssz dynamic: string/hex adapter requires a byte container",
            ))
        }
    };
    match &schema.direct(element)?.value {
        AnyValue::Custom(c) if c.id == "char" => unsigned_adapter(schema, &c.r#type, 8)?,
        _ => unsigned_adapter(schema, element, 8)?,
    }
    Ok((bound, exact))
}
fn bound_bytes(schema: &Schema, ty: &AnyType, len: usize) -> PsszResult<()> {
    let (bound, exact) = byte_container(schema, ty)?;
    if bound.is_some_and(|bound| if exact { len != bound } else { len > bound }) {
        return Err(failure("pssz dynamic: byte container bound exceeded"));
    }
    Ok(())
}


// ── Widths ──────────────────────────────────────────────────────────────────

type Bound = Option<usize>;

fn bound_add(a: Bound, b: usize) -> Bound {
    a.and_then(|a| a.checked_add(b))
}
fn bound_multiply(count: u64, value: Bound) -> Bound {
    if count == 0 {
        return Some(0);
    }
    usize::try_from(count).ok()?.checked_mul(value?)
}
/// `pssz::fit` over a count the schema gives as 64 bits.
fn fit(slots: u64, rest: Bound) -> Bound {
    crate::pssz::fit(usize::try_from(slots).ok()?, rest)
}
fn record_attributes(ty: &AnyType) -> Option<&[Attribute]> {
    match &ty.value {
        AnyValue::Object(o) => Some(&o.attributes),
        AnyValue::Struct(s) => Some(&s.attributes),
        _ => None,
    }
}

/// One encode / decode over a schema. `w` is the explicit profile's width;
/// 0 selects the automatic profile, where every container has its own.
struct Codec<'a> {
    schema: &'a Schema,
    w: usize,
    /// Widths already derived for a container type at a position
    /// (automatic profile), keyed by the resolved type's address in
    /// `schema`, the member count that bounds a plain list, and the cap in
    /// effect.
    widths: RefCell<HashMap<(usize, Option<u64>, Option<u64>), usize>>,
    /// The cap in effect at the value being read or written (automatic
    /// profile): the explicit maxDynamicData of the nearest enclosing record
    /// when it has one, none otherwise (the record has no cap, or the value
    /// is the root, or the inner value of a Nested node). It bounds every
    /// list, array, map, set and tuple beneath the record for the purpose
    /// of its offset width (docs/pssz-spec.md 3.6). A record sets it for
    /// its members and restores it; every other node leaves it alone.
    active_cap: Cell<Option<u64>>,
    /// The count (exact, else max) of the lengthBound of the record member
    /// whose value the next `encode_shape` / `decode_shape` call handles: it
    /// bounds a plain List (or the list of a map / set) reached through
    /// Option, and so selects that list's offset-table width. Set around
    /// the member's call by `encode_members` / `decode_members`, taken by
    /// the callee.
    position_count: Cell<Option<u64>>,
    /// The registry, when it has anything to say about a binary walk.
    handlers: Option<&'a CustomHandlers>,
    /// `NestedBytes::Opaque`: a Nested node is read and written as Bytes.
    opaque: bool,
    /// The error returned is a handler's (a violated invariant, or a
    /// missing handler under a strict registry), not a structural one.
    check_failed: Cell<bool>,
}

impl<'a> Codec<'a> {
    fn new<F: PsszFormat>(schema: &'a Schema) -> PsszResult<Self> {
        Self::with::<F>(schema, Options::default())
    }
    fn with<F: PsszFormat>(schema: &'a Schema, options: Options<'a>) -> PsszResult<Self> {
        schema.check_ir_version()?;
        checked::format::<F>()?;
        Ok(Self {
            schema,
            w: if F::AUTO { 0 } else { F::OFFSET_BYTES },
            widths: RefCell::new(HashMap::new()),
            active_cap: Cell::new(None),
            position_count: Cell::new(None),
            handlers: options.handlers.filter(|handlers| handlers.checks_values()),
            opaque: options.nested == NestedBytes::Opaque,
            check_failed: Cell::new(false),
        })
    }
    /// The automatic-profile codec of the same schema: the profile of the
    /// bytes inside a Nested node and of the bytes a handler sees.
    fn automatic_codec(&self, handlers: bool, opaque: bool) -> Codec<'a> {
        Codec {
            schema: self.schema,
            w: 0,
            widths: RefCell::new(HashMap::new()),
            active_cap: Cell::new(None),
            position_count: Cell::new(None),
            handlers: if handlers { self.handlers } else { None },
            opaque,
            check_failed: Cell::new(false),
        }
    }
    /// The `check`s of the custom nodes `input` passes through on the way to
    /// its wire type, outermost first. A strict registry makes a custom node
    /// with no handler an error.
    fn custom_checks(&self, input: &AnyType) -> PsszResult<Vec<&'a json::Check>> {
        let mut out = Vec::new();
        let Some(handlers) = self.handlers else { return Ok(out) };
        let mut at = input;
        for _ in 0..=4 * self.schema.types.len() + 64 {
            match &at.value {
                AnyValue::Custom(c) if !wire_custom_id(&c.id) => {
                    if c.id != "box"
                        && crate::time::parse_time_schema_id(&c.id).is_none()
                        && !crate::time::reserved_time_schema_id(&c.id)
                    {
                        match handlers.check_of(&c.id) {
                            Ok(Some(check)) => out.push(check),
                            Ok(None) => (),
                            Err(error) => {
                                self.check_failed.set(true);
                                return Err(error);
                            }
                        }
                    }
                    at = &c.r#type;
                }
                AnyValue::Type(reference) => match self.schema.get(&reference.r#type) {
                    Some(found) => at = found,
                    None => break,
                },
                _ => break,
            }
        }
        Ok(out)
    }
    /// Runs `checks` innermost first on the automatic-profile bytes of the
    /// value; `bytes` are those bytes when this codec is the automatic one.
    fn run_checks(
        &self,
        checks: &[&json::Check],
        input: &AnyType,
        value: &Value,
        bytes: &[u8],
    ) -> PsszResult<()> {
        if checks.is_empty() {
            return Ok(());
        }
        // A handler reads the value as a root. Beneath a capped record the
        // bytes in place can have narrower offsets than the root encoding
        // (`active_cap`), so they are re-encoded for it, as under an
        // explicit profile.
        let canonical;
        let bytes = if self.automatic() && self.active_cap.get().is_none() {
            bytes
        } else {
            canonical = self.automatic_codec(false, self.opaque).encode_at(input, value, 0)?;
            &canonical
        };
        for check in checks.iter().rev() {
            if let Err(error) = check(bytes) {
                self.check_failed.set(true);
                return Err(error);
            }
        }
        Ok(())
    }
    fn automatic(&self) -> bool {
        self.w == 0
    }
    fn check(&self, root: &AnyType) -> PsszResult<()> {
        metadata(self.schema, root, self.automatic())
    }
    /// Explicit cap of a resolved record type (automatic profile only).
    fn cap_of(&self, ty: &AnyType) -> PsszResult<Option<u64>> {
        match record_attributes(ty) {
            Some(values) if self.automatic() => parse_cap(values),
            _ => Ok(None),
        }
    }
    /// Whether a value is inline bytes with no offset slot in the bound
    /// bookkeeping (C++ `max_size.hpp` `fixed_payload<T>()`).
    fn fixed_payload(&self, input: &AnyType, active: &mut Vec<usize>) -> PsszResult<bool> {
        let ty = self.schema.wire(input)?;
        let key = ty as *const AnyType as usize;
        if active.contains(&key) {
            return Ok(false);
        }
        active.push(key);
        let result = self.fixed_payload_of(ty, active);
        active.pop();
        result
    }
    fn fixed_payload_of(&self, ty: &AnyType, active: &mut Vec<usize>) -> PsszResult<bool> {
        let members = |members: &[Member], active: &mut Vec<usize>| -> PsszResult<bool> {
            let mut all = true;
            for member in members {
                all &= self.fixed_payload(&member.r#type, active)?;
            }
            Ok(all)
        };
        Ok(match &ty.value {
            AnyValue::Int(_) | AnyValue::Float(_) => true,
            AnyValue::Array(a) => self.fixed_payload(&a.r#type, active)?,
            AnyValue::Custom(c) if c.id == "bool" || c.id == "char" || c.id == "bitvector" => true,
            AnyValue::Custom(c) if byte_adapter_id(&c.id) => {
                let child = self.schema.wire(&c.r#type)?;
                matches!(child.value, AnyValue::Array(_)) && self.fixed_payload(child, active)?
            }
            AnyValue::Struct(s) => members(&s.members, active)?,
            AnyValue::Object(o) if dwnc(&o.attributes) => members(&o.members, active)?,
            _ => false,
        })
    }
    /// The bound the typed codec infers (C++ `max_size.hpp`): a child
    /// reserves its declared cap in full when `reserve_cap`; the type's own
    /// width ignores its own cap. `annotation` is the member's lengthBound;
    /// it reaches a string or list through Option. A recursive type is
    /// unbounded. A type's own header, slots and offset table are counted
    /// at the width the type ends up with (`fit`, docs/pssz-spec.md 3.2).
    fn bound_of(
        &self,
        input: &AnyType,
        annotation: Option<&LengthBound>,
        reserve_cap: bool,
        active: &mut Vec<usize>,
    ) -> PsszResult<Bound> {
        let (ty, boxed) = self.schema.wire_boxed(input)?;
        // A boxed value has no inferred bound (the typed `Box<T>` has none).
        if boxed {
            return Ok(None);
        }
        let key = ty as *const AnyType as usize;
        if active.contains(&key) {
            return Ok(None);
        }
        if reserve_cap {
            if let Some(cap) = self.cap_of(ty)? {
                return Ok(usize::try_from(cap).ok());
            }
        }
        active.push(key);
        let result = self.bound_inner(ty, annotation, active);
        active.pop();
        result
    }
    /// N elements, each its reserved payload, and the offset table when
    /// they are not inline fixed payloads.
    fn bound_elements(
        &self,
        element: &AnyType,
        count: u64,
        active: &mut Vec<usize>,
    ) -> PsszResult<Bound> {
        if count == 0 {
            return Ok(Some(0));
        }
        let each = self.bound_of(element, None, true, active)?;
        let payload = bound_multiply(count, each);
        if self.fixed_payload(element, &mut Vec::new())? {
            return Ok(payload);
        }
        Ok(fit(count, payload))
    }
    fn bound_sequence(
        &self,
        container: &AnyType,
        annotation: Option<&LengthBound>,
        active: &mut Vec<usize>,
    ) -> PsszResult<Bound> {
        match &container.value {
            AnyValue::List(l) => match annotation.and_then(LengthBound::count) {
                Some(count) => self.bound_elements(&l.r#type, count, active),
                None => Ok(None),
            },
            AnyValue::BoundedList(l) => self.bound_elements(&l.r#type, l.maxCount, active),
            AnyValue::Array(a) => self.bound_elements(&a.r#type, a.len, active),
            _ => Ok(None),
        }
    }
    fn bound_inner(
        &self,
        ty: &AnyType,
        annotation: Option<&LengthBound>,
        active: &mut Vec<usize>,
    ) -> PsszResult<Bound> {
        // The header (extensible records), and one slot per variable member.
        let record = |members: &[Member], stable: bool, active: &mut Vec<usize>| {
            let mut slots: u64 = if stable { 0 } else { 1 };
            let mut total: Bound = Some(0);
            for member in members {
                let bound = parse_length_bound(&member.attributes)?;
                let payload = self.bound_of(&member.r#type, bound.as_ref(), true, active)?;
                if !self.fixed_payload(&member.r#type, &mut Vec::new())? {
                    slots += 1;
                }
                total = match (total, payload) {
                    (Some(total), Some(payload)) => total.checked_add(payload),
                    _ => None,
                };
            }
            Ok(fit(slots, total))
        };
        match &ty.value {
            AnyValue::Int(i) => Ok(Some(int_size(i)?)),
            AnyValue::Float(f) => Ok(Some(float_size(f)?)),
            AnyValue::List(_) | AnyValue::BoundedList(_) | AnyValue::Array(_) => {
                self.bound_sequence(ty, annotation, active)
            }
            // One selector byte is reserved for every optional.
            AnyValue::Option(o) => Ok(bound_add(
                self.bound_of(&o.r#type, annotation, true, active)?,
                1,
            )),
            AnyValue::Custom(c) if c.id == "bool" || c.id == "char" => Ok(Some(1)),
            AnyValue::Custom(c) if byte_adapter_id(&c.id) => {
                self.bound_sequence(self.schema.wire(&c.r#type)?, annotation, active)
            }
            // A map / set is bounded as the list of its entries.
            AnyValue::Custom(c) if assoc_id(&c.id) => {
                self.bound_sequence(assoc_shape(self.schema, c)?.list, annotation, active)
            }
            // No bound of its own; a member lengthBound bounds its bytes.
            AnyValue::Nested(_) => {
                Ok(annotation.and_then(LengthBound::count).and_then(|n| usize::try_from(n).ok()))
            }
            AnyValue::Struct(s) => record(&s.members, true, active),
            AnyValue::Object(o) => record(&o.members, dwnc(&o.attributes), active),
            // The extensible positional record of its members: the header,
            // then per member its reserved payload and, when it is not an
            // inline fixed payload, an offset slot.
            AnyValue::Tuple(t) => {
                let mut slots: u64 = 1;
                let mut total: Bound = Some(0);
                for member in &t.members {
                    let payload = self.bound_of(member, None, true, active)?;
                    if !self.fixed_payload(member, &mut Vec::new())? {
                        slots += 1;
                    }
                    total = match (total, payload) {
                        (Some(total), Some(payload)) => total.checked_add(payload),
                        _ => None,
                    };
                }
                Ok(fit(slots, total))
            }
            // One selector byte, then the widest alternative's payload.
            AnyValue::Variant(v) => {
                let mut widest: Bound = Some(0);
                for member in &v.members {
                    let payload = self.bound_of(&member.r#type, None, true, active)?;
                    widest = match (widest, payload) {
                        (Some(widest), Some(payload)) => Some(widest.max(payload)),
                        _ => None,
                    };
                }
                Ok(bound_add(widest, 1))
            }
            // ceil(N / 8) bytes; a bitlist one more for its delimiter bit.
            AnyValue::Custom(c) if c.id == "bitvector" || c.id == "bitlist" => {
                let (count, list) = bit_shape(self.schema, c)?;
                Ok(bound_add(Some(crate::pssz_bits::byte_count(count)), usize::from(list)))
            }
            _ => Ok(None),
        }
    }
    /// The width of a record's header and slots, or of a tuple / list /
    /// array offset table. `ty` is resolved. Explicit profile: the profile's
    /// width. Automatic: the type's own — the width class of its explicit
    /// cap, else of its inferred bound. `count`: for a plain List, the count
    /// the lengthBound of the member that holds it gives (`position_count`);
    /// a list with no bound is 4 wide. Anything that is not a record is also
    /// bounded by the cap in effect (`active_cap`): its width is the class
    /// of the smaller of the two. A record's width is its own wherever it
    /// appears.
    fn width_of(&self, ty: &AnyType, count: Option<u64>) -> PsszResult<usize> {
        if !self.automatic() {
            return Ok(self.w);
        }
        // Only a plain List takes a bound from the member that holds it.
        let count = if matches!(ty.value, AnyValue::List(_)) { count } else { None };
        let limit = if record_attributes(ty).is_some() { None } else { self.active_cap.get() };
        let key = (ty as *const AnyType as usize, count, limit);
        if let Some(width) = self.widths.borrow().get(&key) {
            return Ok(*width);
        }
        let bound = match self.cap_of(ty)? {
            Some(cap) => usize::try_from(cap).ok(),
            None => {
                let annotation = count.map(|count| LengthBound { max: Some(count), ..LengthBound::default() });
                let own = self.bound_of(ty, annotation.as_ref(), false, &mut Vec::new())?;
                match limit.and_then(|limit| usize::try_from(limit).ok()) {
                    Some(limit) if own.is_none_or(|own| own > limit) => Some(limit),
                    _ => own,
                }
            }
        };
        let width = crate::pssz::width_class(bound);
        self.widths.borrow_mut().insert(key, width);
        Ok(width)
    }
    /// Runs `body` with `cap` as the cap in effect.
    fn beneath<R>(&self, cap: Option<u64>, body: impl FnOnce() -> R) -> R {
        let saved = self.active_cap.replace(cap);
        let result = body();
        self.active_cap.set(saved);
        result
    }
    /// Runs `body` for the value of a member whose lengthBound count is
    /// `count`.
    fn at_count<R>(&self, count: Option<u64>, body: impl FnOnce() -> R) -> R {
        self.position_count.set(count);
        let result = body();
        self.position_count.set(None);
        result
    }
    /// Explicit profile: the cap of the top-level type bounds the whole
    /// encoding, as in the typed `Pssz8` / `Pssz16` / `Pssz32` codecs. Nested
    /// caps are not looked up. Nothing to do under the automatic profile,
    /// which checks every record where it is read or written.
    fn check_top_cap(&self, root: &AnyType, size: usize) -> PsszResult<()> {
        if self.automatic() {
            return Ok(());
        }
        let ty = self.schema.wire(root)?;
        match record_attributes(ty).map(parse_cap).transpose()?.flatten() {
            Some(cap) if size as u128 > cap as u128 => Err(failure(
                "pssz dynamic: encoded size exceeds maxDynamicData cap",
            )),
            _ => Ok(()),
        }
    }
    /// A record over its explicit cap is not a value of its type.
    fn check_cap(&self, ty: &AnyType, size: usize) -> PsszResult<()> {
        match self.cap_of(ty)? {
            Some(cap) if size as u128 > cap as u128 => Err(failure(
                "pssz dynamic: encoded size exceeds maxDynamicData cap",
            )),
            _ => Ok(()),
        }
    }

    // ── Encode ──────────────────────────────────────────────────────────────

    /// Every value is encoded through here: its bytes (`encode_shape`),
    /// then the checks of the custom nodes it is a value of.
    fn encode_at(&self, ty: &AnyType, value: &Value, n: usize) -> PsszResult<Vec<u8>> {
        if self.handlers.is_none() {
            return self.encode_shape(ty, value, n);
        }
        depth(n)?;
        let checks = self.custom_checks(ty)?;
        let bytes = self.encode_shape(ty, value, n)?;
        self.run_checks(&checks, ty, value, &bytes)?;
        Ok(bytes)
    }
    /// The inner value of a Nested node: always the automatic profile.
    fn nested_encode(&self, ty: &AnyType, value: &Value, n: usize) -> PsszResult<Vec<u8>> {
        if self.automatic() {
            // The inner value is a root of its own: no cap reaches into it.
            return self.beneath(None, || self.encode_at(ty, value, n));
        }
        let codec = self.automatic_codec(true, self.opaque);
        let result = codec.encode_at(ty, value, n);
        self.check_failed.set(self.check_failed.get() | codec.check_failed.get());
        result
    }
    fn nested_decode(&self, ty: &AnyType, bytes: &[u8], n: usize) -> PsszResult<Value> {
        if self.automatic() {
            return self.beneath(None, || self.decode_at(ty, bytes, n));
        }
        let codec = self.automatic_codec(true, self.opaque);
        let result = codec.decode_at(ty, bytes, n);
        self.check_failed.set(self.check_failed.get() | codec.check_failed.get());
        result
    }
    /// The lengthBound of a Nested member (through Option) against its
    /// encoded form `wire`. An absent optional has no length.
    fn nested_bound(&self, ty: &AnyType, mut wire: &[u8], bound: &LengthBound) -> PsszResult<()> {
        let mut at = self.schema.wire(ty)?;
        while let AnyValue::Option(o) = &at.value {
            let selector = can_be_empty(self.schema, &o.r#type, 0)?;
            at = self.schema.wire(&o.r#type)?;
            if wire.is_empty() || (selector && wire[0] == 0) {
                return Ok(());
            }
            if selector {
                wire = &wire[1..];
            }
        }
        if bound.admits(wire.len() as u64) {
            Ok(())
        } else {
            Err(failure("pssz dynamic: member length bound violated"))
        }
    }
    fn encode_shape(&self, ty: &AnyType, value: &Value, n: usize) -> PsszResult<Vec<u8>> {
        // The lengthBound count of the member this value is (see
        // `position_count`): it reaches a list through Option only.
        let member_count = self.position_count.take();
        depth(n)?;
        let schema = self.schema;
        let ty = schema.wire(ty)?;
        let mut out = Vec::new();
        match (&ty.value, value) {
            (AnyValue::Int(i), Value::Bytes(v)) if i.bits == 256 => {
                if int_size(i)? != v.len() {
                    return Err(failure(
                        "pssz dynamic: 256-bit integer needs 32 little-endian bytes",
                    ));
                }
                out.extend_from_slice(v);
            }
            (AnyValue::Int(i), Value::Unsigned(v)) if !i.isSigned && i.bits != 256 => {
                let size = int_size(i)?;
                if i.bits < 128 && *v >= (1u128 << i.bits) {
                    return Err(failure("pssz dynamic: unsigned integer overflow"));
                }
                out.extend_from_slice(&v.to_le_bytes()[..size]);
            }
            (AnyValue::Int(i), Value::Signed(v)) if i.isSigned => {
                let size = int_size(i)?;
                if i.bits < 128 {
                    let limit = 1i128 << (i.bits - 1);
                    if *v < -limit || *v >= limit {
                        return Err(failure("pssz dynamic: signed integer overflow"));
                    }
                }
                out.extend_from_slice(&v.to_le_bytes()[..size]);
            }
            (AnyValue::Float(f), Value::Float32(v))
                if f.exp == 8 && matches!(f.mantissa, 23 | 24) =>
            {
                out.extend_from_slice(&v.to_le_bytes())
            }
            (AnyValue::Float(f), Value::Float64(v))
                if f.exp == 11 && matches!(f.mantissa, 52 | 53) =>
            {
                out.extend_from_slice(&v.to_le_bytes())
            }
            (AnyValue::Custom(c), Value::Bool(v)) if c.id == "bool" => {
                unsigned_adapter(schema, &c.r#type, 1)?;
                out.push(u8::from(*v));
            }
            (AnyValue::Custom(c), Value::Unsigned(v)) if c.id == "char" => {
                unsigned_adapter(schema, &c.r#type, 8)?;
                out.push(
                    u8::try_from(*v).map_err(|_| failure("pssz dynamic: char byte overflow"))?,
                );
            }
            (AnyValue::Custom(c), Value::String(v)) if c.id == "string" || c.id == "utf8" => {
                bound_bytes(schema, &c.r#type, v.len())?;
                out.extend_from_slice(v.as_bytes());
            }
            // A raw string is any bytes: one that is not UTF-8 is carried
            // as `Value::Bytes`.
            (AnyValue::Custom(c), Value::Bytes(v)) if c.id == "string" => {
                bound_bytes(schema, &c.r#type, v.len())?;
                out.extend_from_slice(v);
            }
            (AnyValue::Custom(c), Value::Bytes(v)) if c.id == "hex" => {
                bound_bytes(schema, &c.r#type, v.len())?;
                if !v.is_empty() {
                    depth(n + 1)?;
                }
                out.extend_from_slice(v);
            }
            (AnyValue::Custom(c), Value::Bits { bytes, bit_count })
                if c.id == "bitvector" || c.id == "bitlist" =>
            {
                let (bound, list) = bit_shape(schema, c)?;
                let count = usize::try_from(*bit_count)
                    .map_err(|_| failure("pssz dynamic: bit count exceeds platform capacity"))?;
                if list {
                    out = crate::pssz_bits::encode_list(bytes, count, bound)?;
                } else {
                    if count != bound {
                        return Err(failure("pssz dynamic: bitvector logical count mismatch"));
                    }
                    crate::pssz_bits::validate_data(bytes, count)?;
                    out.extend_from_slice(bytes);
                }
            }
            (AnyValue::Option(o), Value::Optional(v)) => {
                if can_be_empty(schema, &o.r#type, 0)? {
                    out.push(u8::from(v.is_some()));
                }
                if let Some(inner) = v {
                    out.extend(self.at_count(member_count, || self.encode_at(&o.r#type, inner, n + 1))?);
                }
            }
            // The members lie beneath this record's cap, or beneath none.
            (AnyValue::Struct(s), Value::Record(values)) => {
                let w = self.width_of(ty, None)?;
                let cap = self.cap_of(ty)?;
                self.beneath(cap, || self.encode_members(&s.members, values, true, w, n, &mut out))?;
                self.check_cap(ty, out.len())?;
            }
            (AnyValue::Object(o), Value::Record(values)) => {
                let w = self.width_of(ty, None)?;
                let cap = self.cap_of(ty)?;
                self.beneath(cap, || {
                    self.encode_members(&o.members, values, dwnc(&o.attributes), w, n, &mut out)
                })?;
                self.check_cap(ty, out.len())?;
            }
            (AnyValue::Tuple(t), Value::Sequence(values)) => {
                if t.members.len() != values.len() {
                    return Err(failure("pssz dynamic: tuple field count mismatch"));
                }
                let mut fields = Vec::new();
                let omitted = t
                    .members
                    .iter()
                    .zip(values)
                    .map(|(ty, value)| {
                        Ok(layout_of(schema, ty)?.optional
                            && matches!(value, Value::Optional(None)))
                    })
                    .collect::<PsszResult<Vec<_>>>()?;
                let keep = checked::record_kept_fields(&omitted, false);
                for (ty, value) in t.members.iter().zip(values).take(keep) {
                    fields.push(self.encoded_field(ty, value, n + 1)?);
                }
                checked::encode_record_w(self.width_of(ty, None)?, &fields, false, &mut out)?;
            }
            (AnyValue::List(l), Value::Sequence(values)) => {
                let w = self.width_of(ty, member_count)?;
                self.encode_sequence(&l.r#type, values, None, false, w, n, &mut out)?
            }
            (AnyValue::BoundedList(l), Value::Sequence(values)) => {
                let w = self.width_of(ty, None)?;
                self.encode_sequence(&l.r#type, values, Some(l.maxCount), false, w, n, &mut out)?
            }
            (AnyValue::Array(a), Value::Sequence(values)) => {
                let w = self.width_of(ty, None)?;
                self.encode_sequence(&a.r#type, values, Some(a.len), true, w, n, &mut out)?
            }
            (AnyValue::Variant(v), Value::Variant { selector, value }) => {
                if v.members.len() > 256 || *selector >= v.members.len() {
                    return Err(failure("pssz dynamic: variant selector out of range"));
                }
                out.push(*selector as u8);
                out.extend(self.encode_at(&v.members[*selector].r#type, value, n + 1)?);
            }
            (AnyValue::Custom(c), Value::Sequence(values)) if assoc_id(&c.id) => {
                out = self.encode_assoc(c, values, n, member_count)?;
            }
            (AnyValue::Nested(_), Value::Bytes(bytes)) if self.opaque => {
                if !bytes.is_empty() {
                    depth(n + 1)?;
                }
                out.extend_from_slice(bytes);
            }
            (AnyValue::Nested(_), _) if self.opaque => {
                return Err(failure(
                    "pssz dynamic: bytes value required for an opaque nested value",
                ))
            }
            (AnyValue::Nested(nested), _) => {
                out = self.nested_encode(&nested.r#type, value, n + 1)?;
            }
            _ => {
                return Err(failure(
                    "pssz dynamic: value does not match supported schema",
                ))
            }
        }
        Ok(out)
    }

    // ── Maps, sets and the key encoding ─────────────────────────────────────

    /// The `key` encoding (docs/key-spec.md) of a dynamic value, appended.
    fn key_at(&self, input: &AnyType, value: &Value, out: &mut Vec<u8>, n: usize) -> PsszResult<()> {
        depth(n)?;
        let schema = self.schema;
        let (ty, boxed) = schema.wire_boxed(input)?;
        const NONE: &str = "pssz dynamic: this type has no key encoding";
        const MISMATCH: &str = "pssz dynamic: value does not match supported schema";
        if boxed {
            return Err(failure("pssz dynamic: a boxed type has no key encoding"));
        }
        match (&ty.value, value) {
            (AnyValue::Int(i), _) if i.bits == 1 => return Err(failure(NONE)),
            (AnyValue::Int(i), Value::Bytes(v)) if i.bits == 256 && !i.isSigned && v.len() == 32 => {
                out.extend(v.iter().rev());
            }
            (AnyValue::Int(i), Value::Unsigned(v)) if !i.isSigned && i.bits != 256 => {
                let size = int_size(i)?;
                if i.bits < 128 && *v >= (1u128 << i.bits) {
                    return Err(failure("pssz dynamic: unsigned integer overflow"));
                }
                out.extend_from_slice(&v.to_be_bytes()[16 - size..]);
            }
            (AnyValue::Int(i), Value::Signed(v)) if i.isSigned => {
                let size = int_size(i)?;
                if i.bits < 128 {
                    let limit = 1i128 << (i.bits - 1);
                    if *v < -limit || *v >= limit {
                        return Err(failure("pssz dynamic: signed integer overflow"));
                    }
                }
                let raw = (*v as u128) ^ (1u128 << (i.bits - 1));
                out.extend_from_slice(&raw.to_be_bytes()[16 - size..]);
            }
            // -0 becomes +0; negative: every bit flipped; else the sign bit.
            (AnyValue::Float(f), Value::Float32(v)) if float_size(f)? == 4 => {
                let mut bits = v.to_bits();
                if bits == 1 << 31 {
                    bits = 0;
                }
                bits = if bits & (1 << 31) != 0 { !bits } else { bits ^ (1 << 31) };
                out.extend_from_slice(&bits.to_be_bytes());
            }
            (AnyValue::Float(f), Value::Float64(v)) if float_size(f)? == 8 => {
                let mut bits = v.to_bits();
                if bits == 1 << 63 {
                    bits = 0;
                }
                bits = if bits & (1 << 63) != 0 { !bits } else { bits ^ (1 << 63) };
                out.extend_from_slice(&bits.to_be_bytes());
            }
            (AnyValue::Custom(c), Value::Bool(v)) if c.id == "bool" => out.push(u8::from(*v)),
            (AnyValue::Custom(c), Value::Unsigned(v)) if c.id == "char" && *v <= 255 => {
                out.push(*v as u8)
            }
            (AnyValue::Custom(c), Value::String(_) | Value::Bytes(_)) if byte_adapter_id(&c.id) => {
                // A string / byte list: escaped and terminated. A fixed byte
                // array: its bytes.
                let bytes: &[u8] = match value {
                    Value::String(text) if c.id != "hex" => text.as_bytes(),
                    Value::Bytes(bytes) if c.id != "utf8" => bytes,
                    _ => return Err(failure(MISMATCH)),
                };
                bound_bytes(schema, &c.r#type, bytes.len())?;
                if matches!(schema.wire(&c.r#type)?.value, AnyValue::Array(_)) {
                    out.extend_from_slice(bytes);
                } else {
                    key_escaped(bytes, out);
                }
            }
            (AnyValue::Custom(c), Value::Bits { bytes, bit_count }) if c.id == "bitvector" => {
                let (count, _) = bit_shape(schema, c)?;
                if *bit_count != count as u64 {
                    return Err(failure("pssz dynamic: bitvector logical count mismatch"));
                }
                crate::pssz_bits::validate_data(bytes, count)?;
                out.extend_from_slice(bytes);
            }
            (AnyValue::Custom(c), Value::Sequence(items)) if assoc_id(&c.id) => {
                let shape = assoc_shape(schema, c)?;
                let order = self.assoc_order(&shape, items)?;
                self.key_list(shape.element, items, Some(&order), out, n)?;
            }
            (AnyValue::Option(_), Value::Optional(None)) => out.push(0),
            (AnyValue::Option(o), Value::Optional(Some(inner))) => {
                out.push(1);
                self.key_at(&o.r#type, inner, out, n + 1)?;
            }
            (AnyValue::List(l), Value::Sequence(items)) => {
                self.key_list(&l.r#type, items, None, out, n)?
            }
            (AnyValue::BoundedList(l), Value::Sequence(items)) => {
                if items.len() as u128 > l.maxCount as u128 {
                    return Err(failure("pssz dynamic: sequence count bound exceeded"));
                }
                self.key_list(&l.r#type, items, None, out, n)?
            }
            (AnyValue::Array(a), Value::Sequence(items)) => {
                if items.len() as u128 != a.len as u128 {
                    return Err(failure("pssz dynamic: sequence count bound exceeded"));
                }
                for item in items {
                    self.key_at(&a.r#type, item, out, n + 1)?;
                }
            }
            (AnyValue::Tuple(t), Value::Sequence(items)) => {
                if items.len() != t.members.len() {
                    return Err(failure("pssz dynamic: tuple field count mismatch"));
                }
                for (member, item) in t.members.iter().zip(items) {
                    self.key_at(member, item, out, n + 1)?;
                }
            }
            (AnyValue::Struct(s), Value::Record(fields)) => {
                self.key_members(&s.members, fields, out, n)?
            }
            (AnyValue::Object(o), Value::Record(fields)) => {
                self.key_members(&o.members, fields, out, n)?
            }
            (AnyValue::Variant(v), Value::Variant { selector, value }) => {
                if v.members.len() > 256 || *selector >= v.members.len() {
                    return Err(failure("pssz dynamic: variant selector out of range"));
                }
                out.push(*selector as u8);
                self.key_at(&v.members[*selector].r#type, value, out, n + 1)?;
            }
            // The key of the byte list holding the inner value's canonical
            // encoding (the octet framing): the typed key of a nested value.
            (AnyValue::Nested(_), Value::Bytes(bytes)) if self.opaque => key_escaped(bytes, out),
            (AnyValue::Nested(_), _) if self.opaque => {
                return Err(failure(
                    "pssz dynamic: bytes value required for an opaque nested value",
                ))
            }
            (AnyValue::Nested(nested), _) => {
                key_escaped(&self.nested_encode(&nested.r#type, value, n + 1)?, out)
            }
            (AnyValue::Custom(c), _) if c.id == "bitlist" || c.id == "unknown" => {
                return Err(failure(NONE))
            }
            (AnyValue::Resource(_), _) => return Err(failure(NONE)),
            _ => return Err(failure(MISMATCH)),
        }
        Ok(())
    }
    /// A list / map / set payload: the octet framing for a list of octets
    /// (u8, i8 or char, reached through aliases only), else `01` + element
    /// per element and `00`.
    fn key_list(
        &self,
        element: &AnyType,
        items: &[Value],
        order: Option<&[usize]>,
        out: &mut Vec<u8>,
        n: usize,
    ) -> PsszResult<()> {
        let octet = match &self.schema.resolve(element)?.value {
            AnyValue::Int(i) => i.bits == 8,
            AnyValue::Custom(c) => c.id == "char",
            _ => false,
        };
        let mut one = Vec::new();
        for i in 0..items.len() {
            let item = &items[order.map_or(i, |order| order[i])];
            if !octet {
                out.push(1);
                self.key_at(element, item, out, n + 1)?;
                continue;
            }
            one.clear();
            self.key_at(element, item, &mut one, n + 1)?;
            out.push(one[0]);
            if one[0] == 0 {
                out.push(1);
            }
        }
        out.push(0);
        if octet {
            out.push(0);
        }
        Ok(())
    }
    /// The members in schema order; an absent Option member is None.
    fn key_members(
        &self,
        members: &[Member],
        fields: &[(String, Value)],
        out: &mut Vec<u8>,
        n: usize,
    ) -> PsszResult<()> {
        for (name, _) in fields {
            if fields.iter().filter(|(other, _)| other == name).count() != 1
                || !members.iter().any(|m| &m.name == name)
            {
                return Err(failure("pssz dynamic: duplicate or unknown value field"));
            }
        }
        let absent = Value::Optional(None);
        for member in members {
            let value = match fields.iter().find(|(name, _)| name == &member.name) {
                Some((_, value)) => value,
                None if layout_of(self.schema, &member.r#type)?.optional => &absent,
                None => return Err(failure("pssz dynamic: missing required value field")),
            };
            self.key_at(&member.r#type, value, out, n + 1)?;
        }
        Ok(())
    }
    /// The key bytes of every entry's key, in `items` order.
    fn assoc_keys(&self, shape: &AssocShape<'_>, items: &[Value]) -> PsszResult<Vec<Vec<u8>>> {
        items
            .iter()
            .map(|item| {
                let key = match (shape.map, item) {
                    (false, _) => item,
                    (true, Value::Sequence(entry)) if entry.len() == 2 => &entry[0],
                    _ => return Err(failure("pssz dynamic: a map entry is a [key, value] pair")),
                };
                let mut bytes = Vec::new();
                self.key_at(shape.key, key, &mut bytes, 0)?;
                Ok(bytes)
            })
            .collect()
    }
    /// The canonical order of a map / set value's entries: indices into
    /// `items`, ascending by key bytes. Duplicate keys are an error
    /// (`MAP_DUPLICATE` / `SET_DUPLICATE`).
    fn assoc_order(&self, shape: &AssocShape<'_>, items: &[Value]) -> PsszResult<Vec<usize>> {
        let keys = self.assoc_keys(shape, items)?;
        let mut order: Vec<usize> = (0..items.len()).collect();
        order.sort_by(|a, b| keys[*a].cmp(&keys[*b]));
        if order.windows(2).any(|pair| keys[pair[0]] == keys[pair[1]]) {
            return Err(failure(if shape.map { MAP_DUPLICATE } else { SET_DUPLICATE }));
        }
        Ok(order)
    }
    fn encode_assoc(
        &self,
        custom: &Custom,
        items: &[Value],
        n: usize,
        count: Option<u64>,
    ) -> PsszResult<Vec<u8>> {
        let shape = assoc_shape(self.schema, custom)?;
        let order = self.assoc_order(&shape, items)?;
        let w = self.width_of(shape.list, count)?;
        let mut out = Vec::new();
        // Values that are already in canonical order (anything a decoder or
        // a text reader produced) are encoded as they are.
        if order.windows(2).all(|pair| pair[0] < pair[1]) {
            self.encode_sequence(shape.element, items, shape.max, false, w, n, &mut out)?;
        } else {
            let sorted: Vec<Value> = order.iter().map(|index| items[*index].clone()).collect();
            self.encode_sequence(shape.element, &sorted, shape.max, false, w, n, &mut out)?;
        }
        Ok(out)
    }
    fn decode_assoc(&self, custom: &Custom, bytes: &[u8], n: usize, count: Option<u64>) -> PsszResult<Value> {
        let shape = assoc_shape(self.schema, custom)?;
        let w = self.width_of(shape.list, count)?;
        let items = self.decode_sequence(shape.element, bytes, shape.max, false, w, n)?;
        // The order is that of the bytes read. A decoded Nested value stands
        // for its canonical encoding, which is not what a compatible read
        // saw when the inner bytes carry unknown fields: keys holding a
        // Nested node are taken from the bytes as they are.
        let keys = if !self.opaque && key_has_nested(self.schema, shape.key, &mut Vec::new())? {
            let raw = Codec {
                schema: self.schema,
                w: self.w,
                widths: RefCell::new(HashMap::new()),
                // The same position: the list is read at the same width.
                active_cap: Cell::new(self.active_cap.get()),
                position_count: Cell::new(None),
                handlers: None,
                opaque: true,
                check_failed: Cell::new(false),
            };
            let wire = raw.decode_sequence(shape.element, bytes, shape.max, false, w, n)?;
            raw.assoc_keys(&shape, &wire)?
        } else {
            self.assoc_keys(&shape, &items)?
        };
        if keys.windows(2).any(|pair| pair[0] >= pair[1]) {
            return Err(failure(if shape.map { MAP_ORDER } else { SET_ORDER }));
        }
        Ok(Value::Sequence(items))
    }
    fn encoded_field(&self, ty: &AnyType, value: &Value, n: usize) -> PsszResult<(Layout, Vec<u8>)> {
        let mut field = layout_of(self.schema, ty)?;
        field.omit = field.optional && matches!(value, Value::Optional(None));
        let bytes = self.encode_at(ty, value, n)?;
        if field.fixed && bytes.len() != field.size {
            return Err(failure("pssz dynamic: fixed field encoding mismatch"));
        }
        Ok((field, bytes))
    }
    fn encode_members(
        &self,
        members: &[Member],
        values: &[(String, Value)],
        stable: bool,
        w: usize,
        n: usize,
        out: &mut Vec<u8>,
    ) -> PsszResult<()> {
        for (name, _) in values {
            if values.iter().filter(|(other, _)| other == name).count() != 1
                || !members.iter().any(|m| &m.name == name)
            {
                return Err(failure("pssz dynamic: duplicate or unknown value field"));
            }
        }
        let missing = Value::Optional(None);
        let mut child_values = Vec::new();
        let mut omitted = Vec::new();
        for member in members {
            let optional = layout_of(self.schema, &member.r#type)?.optional;
            let value = match values.iter().find(|(name, _)| name == &member.name) {
                Some((_, value)) => value,
                None if optional => &missing,
                None => return Err(failure("pssz dynamic: missing required value field")),
            };
            // A Nested member's bound counts its encoded bytes: checked below.
            let nested = nested_member(self.schema, &member.r#type)?;
            if !nested
                && parse_length_bound(&member.attributes)?.is_some_and(|b| !b.admits_value(value))
            {
                return Err(failure("pssz dynamic: member length bound violated"));
            }
            omitted.push(optional && matches!(value, Value::Optional(None)));
            child_values.push(value);
        }
        let keep = checked::record_kept_fields(&omitted, stable);
        let mut fields = Vec::new();
        for (member, value) in members.iter().zip(child_values).take(keep) {
            let count = parse_length_bound(&member.attributes)?.and_then(|bound| bound.count());
            let field = self.at_count(count, || self.encoded_field(&member.r#type, value, n + 1))?;
            if nested_member(self.schema, &member.r#type)? {
                if let Some(bound) = parse_length_bound(&member.attributes)? {
                    self.nested_bound(&member.r#type, &field.1, &bound)?;
                }
            }
            fields.push(field);
        }
        checked::encode_record_w(w, &fields, stable, out)
    }
    #[allow(clippy::too_many_arguments)]
    fn encode_sequence(
        &self,
        ty: &AnyType,
        values: &[Value],
        count: Option<u64>,
        exact: bool,
        w: usize,
        n: usize,
        out: &mut Vec<u8>,
    ) -> PsszResult<()> {
        if count.is_some_and(|count| {
            if exact {
                values.len() as u128 != count as u128
            } else {
                values.len() as u128 > count as u128
            }
        }) {
            return Err(failure("pssz dynamic: sequence count bound exceeded"));
        }
        let element = layout_of(self.schema, ty)?;
        if !exact && element.fixed && element.size == 0 && !values.is_empty() {
            return Err(failure("pssz dynamic: ambiguous nonempty zero-size list"));
        }
        let mut fields = Vec::new();
        for value in values {
            fields.push(self.encoded_field(ty, value, n + 1)?);
        }
        checked::encode_record_w(w, &fields, true, out)
    }

    // ── Decode ──────────────────────────────────────────────────────────────

    /// Every value is decoded through here: its structure (`decode_shape`),
    /// then the checks of the custom nodes it is a value of.
    fn decode_at(&self, ty: &AnyType, bytes: &[u8], n: usize) -> PsszResult<Value> {
        if self.handlers.is_none() {
            return self.decode_shape(ty, bytes, n);
        }
        depth(n)?;
        let checks = self.custom_checks(ty)?;
        let value = self.decode_shape(ty, bytes, n)?;
        self.run_checks(&checks, ty, &value, bytes)?;
        Ok(value)
    }
    fn decode_shape(&self, ty: &AnyType, bytes: &[u8], n: usize) -> PsszResult<Value> {
        // The lengthBound count of the member this value is (see
        // `position_count`).
        let member_count = self.position_count.take();
        depth(n)?;
        let schema = self.schema;
        let ty = schema.wire(ty)?;
        Ok(match &ty.value {
            AnyValue::Int(i) => {
                let size = int_size(i)?;
                if bytes.len() != size {
                    return Err(failure("pssz dynamic: integer span mismatch"));
                }
                if i.bits == 256 {
                    return Ok(Value::Bytes(bytes.to_vec()));
                }
                let mut raw = [0u8; 16];
                raw[..size].copy_from_slice(bytes);
                if i.isSigned {
                    if bytes[size - 1] & 128 != 0 {
                        raw[size..].fill(255);
                    }
                    let v = i128::from_le_bytes(raw);
                    if i.bits == 1 && !(-1..=0).contains(&v) {
                        return Err(failure("pssz dynamic: signed integer overflow"));
                    }
                    Value::Signed(v)
                } else {
                    let v = u128::from_le_bytes(raw);
                    if i.bits == 1 && v > 1 {
                        return Err(failure("pssz dynamic: one-bit integer overflow"));
                    }
                    Value::Unsigned(v)
                }
            }
            AnyValue::Float(f) => match (f.exp, f.mantissa, bytes.len()) {
                (8, 23 | 24, 4) => Value::Float32(f32::from_le_bytes(
                    bytes
                        .try_into()
                        .map_err(|_| failure("pssz dynamic: float span mismatch"))?,
                )),
                (11, 52 | 53, 8) => Value::Float64(f64::from_le_bytes(
                    bytes
                        .try_into()
                        .map_err(|_| failure("pssz dynamic: float span mismatch"))?,
                )),
                _ => return Err(failure("pssz dynamic: unsupported float or span mismatch")),
            },
            AnyValue::Custom(c) if c.id == "bool" => {
                unsigned_adapter(schema, &c.r#type, 1)?;
                match bytes {
                    [0] => Value::Bool(false),
                    [1] => Value::Bool(true),
                    _ => return Err(failure("pssz dynamic: invalid bool")),
                }
            }
            AnyValue::Custom(c) if c.id == "char" => {
                unsigned_adapter(schema, &c.r#type, 8)?;
                match bytes {
                    [value] => Value::Unsigned(u128::from(*value)),
                    _ => return Err(failure("pssz dynamic: char span mismatch")),
                }
            }
            AnyValue::Custom(c) if c.id == "utf8" => {
                bound_bytes(schema, &c.r#type, bytes.len())?;
                Value::String(
                    std::str::from_utf8(bytes)
                        .map_err(|_| failure("pssz dynamic: invalid UTF-8"))?
                        .into(),
                )
            }
            // A raw string: any bytes are valid. Text when they are UTF-8,
            // the bytes themselves otherwise.
            AnyValue::Custom(c) if c.id == "string" => {
                bound_bytes(schema, &c.r#type, bytes.len())?;
                match std::str::from_utf8(bytes) {
                    Ok(text) => Value::String(text.into()),
                    Err(_) => Value::Bytes(bytes.into()),
                }
            }
            AnyValue::Custom(c) if c.id == "hex" => {
                bound_bytes(schema, &c.r#type, bytes.len())?;
                if !bytes.is_empty() {
                    depth(n + 1)?;
                }
                Value::Bytes(bytes.into())
            }
            AnyValue::Custom(c) if c.id == "bitvector" || c.id == "bitlist" => {
                let (count, list) = bit_shape(schema, c)?;
                if list {
                    let (data, count) = crate::pssz_bits::decode_list(bytes, count)?;
                    Value::Bits {
                        bytes: data,
                        bit_count: count as u64,
                    }
                } else {
                    crate::pssz_bits::validate_data(bytes, count)?;
                    Value::Bits {
                        bytes: bytes.to_vec(),
                        bit_count: count as u64,
                    }
                }
            }
            AnyValue::Option(o) => {
                if can_be_empty(schema, &o.r#type, 0)? {
                    match bytes.split_first() {
                        Some((&0, [])) => Value::Optional(None),
                        Some((&1, payload)) => Value::Optional(Some(Box::new(
                            self.at_count(member_count, || self.decode_at(&o.r#type, payload, n + 1))?,
                        ))),
                        _ => {
                            return Err(failure(
                                "pssz dynamic: invalid optional selector or payload",
                            ))
                        }
                    }
                } else if bytes.is_empty() {
                    Value::Optional(None)
                } else {
                    Value::Optional(Some(Box::new(
                        self.at_count(member_count, || self.decode_at(&o.r#type, bytes, n + 1))?,
                    )))
                }
            }
            AnyValue::Struct(s) => {
                self.check_cap(ty, bytes.len())?;
                let w = self.width_of(ty, None)?;
                let cap = self.cap_of(ty)?;
                Value::Record(self.beneath(cap, || self.decode_members(&s.members, bytes, true, w, n))?)
            }
            AnyValue::Object(o) => {
                self.check_cap(ty, bytes.len())?;
                let w = self.width_of(ty, None)?;
                let cap = self.cap_of(ty)?;
                Value::Record(self.beneath(cap, || {
                    self.decode_members(&o.members, bytes, dwnc(&o.attributes), w, n)
                })?)
            }
            AnyValue::Tuple(t) => {
                let layouts = t
                    .members
                    .iter()
                    .map(|ty| layout_of(schema, ty))
                    .collect::<PsszResult<Vec<_>>>()?;
                let spans = checked::record_spans_w(self.width_of(ty, None)?, &layouts, bytes, false)?;
                let values = t
                    .members
                    .iter()
                    .zip(spans)
                    .map(|(ty, span)| match span {
                        Some(span) => self.decode_at(ty, &bytes[span], n + 1),
                        None => Ok(Value::Optional(None)),
                    })
                    .collect::<PsszResult<Vec<_>>>()?;
                Value::Sequence(values)
            }
            AnyValue::List(l) => {
                let w = self.width_of(ty, member_count)?;
                Value::Sequence(self.decode_sequence(&l.r#type, bytes, None, false, w, n)?)
            }
            AnyValue::BoundedList(l) => {
                let w = self.width_of(ty, None)?;
                Value::Sequence(self.decode_sequence(
                    &l.r#type,
                    bytes,
                    Some(l.maxCount),
                    false,
                    w,
                    n,
                )?)
            }
            AnyValue::Array(a) => {
                let w = self.width_of(ty, None)?;
                Value::Sequence(self.decode_sequence(&a.r#type, bytes, Some(a.len), true, w, n)?)
            }
            AnyValue::Variant(v) => {
                let (&selector, payload) = bytes
                    .split_first()
                    .ok_or(failure("pssz dynamic: variant selector missing"))?;
                let member = v
                    .members
                    .get(selector as usize)
                    .ok_or(failure("pssz dynamic: variant selector out of range"))?;
                if v.members.len() > 256 {
                    return Err(failure("pssz dynamic: too many variant alternatives"));
                }
                Value::Variant {
                    selector: selector as usize,
                    value: Box::new(self.decode_at(&member.r#type, payload, n + 1)?),
                }
            }
            AnyValue::Custom(c) if assoc_id(&c.id) => self.decode_assoc(c, bytes, n, member_count)?,
            AnyValue::Nested(_) if self.opaque => {
                if !bytes.is_empty() {
                    depth(n + 1)?;
                }
                Value::Bytes(bytes.to_vec())
            }
            AnyValue::Nested(nested) => self.nested_decode(&nested.r#type, bytes, n + 1)?,
            _ => {
                return Err(failure(
                    "pssz dynamic: unsupported IR type or custom adapter",
                ))
            }
        })
    }
    fn decode_members(
        &self,
        members: &[Member],
        bytes: &[u8],
        stable: bool,
        w: usize,
        n: usize,
    ) -> PsszResult<Vec<(String, Value)>> {
        let layouts = members
            .iter()
            .map(|m| layout_of(self.schema, &m.r#type))
            .collect::<PsszResult<Vec<_>>>()?;
        let spans = checked::record_spans_w(w, &layouts, bytes, stable)?;
        members
            .iter()
            .zip(spans)
            .map(|(member, span)| {
                let bound = parse_length_bound(&member.attributes)?;
                let nested = bound.is_some() && nested_member(self.schema, &member.r#type)?;
                let value = match span {
                    Some(span) => {
                        // A Nested member's bound counts its encoded bytes.
                        if let (true, Some(bound)) = (nested, &bound) {
                            self.nested_bound(&member.r#type, &bytes[span.clone()], bound)?;
                        }
                        let count = bound.as_ref().and_then(|bound| bound.count());
                        self.at_count(count, || self.decode_at(&member.r#type, &bytes[span], n + 1))?
                    }
                    None => Value::Optional(None),
                };
                if !nested && bound.is_some_and(|b| !b.admits_value(&value)) {
                    return Err(failure("pssz dynamic: member length bound violated"));
                }
                Ok((member.name.clone(), value))
            })
            .collect()
    }
    fn decode_sequence(
        &self,
        ty: &AnyType,
        bytes: &[u8],
        bound: Option<u64>,
        exact: bool,
        w: usize,
        n: usize,
    ) -> PsszResult<Vec<Value>> {
        let element = layout_of(self.schema, ty)?;
        let count = if exact {
            usize::try_from(bound.ok_or(failure("pssz dynamic: fixed array count missing"))?)
                .map_err(|_| failure("pssz dynamic: array count overflow"))?
        } else if bytes.is_empty() {
            0
        } else if element.fixed {
            if element.size == 0 || bytes.len() % element.size != 0 {
                return Err(failure("pssz dynamic: fixed list span mismatch"));
            }
            bytes.len() / element.size
        } else {
            let first = checked::read_offset_w(bytes, w)?;
            if first == 0 || first % w != 0 || first > bytes.len() {
                return Err(failure("pssz dynamic: variable list table mismatch"));
            }
            first / w
        };
        if bound.is_some_and(|bound| {
            if exact {
                count as u128 != bound as u128
            } else {
                count as u128 > bound as u128
            }
        }) {
            return Err(failure("pssz dynamic: list count bound exceeded"));
        }
        let mut spans = Vec::new();
        if element.fixed {
            if count.checked_mul(element.size) != Some(bytes.len()) {
                return Err(failure("pssz dynamic: fixed array span mismatch"));
            }
            spans
                .try_reserve(count)
                .map_err(|_| failure("pssz dynamic: list allocation overflow"))?;
            for i in 0..count {
                spans.push(i * element.size..(i + 1) * element.size);
            }
        } else {
            let table = count
                .checked_mul(w)
                .ok_or(failure("pssz dynamic: list table overflow"))?;
            if table > bytes.len() || (count == 0 && !bytes.is_empty()) {
                return Err(failure("pssz dynamic: list table mismatch"));
            }
            let mut previous = table;
            for i in 0..count {
                let begin = checked::read_offset_w(&bytes[i * w..], w)?;
                let end = if i + 1 < count {
                    checked::read_offset_w(&bytes[(i + 1) * w..], w)?
                } else {
                    bytes.len()
                };
                if (i == 0 && begin != table)
                    || begin < previous
                    || begin > end
                    || end > bytes.len()
                {
                    return Err(failure("pssz dynamic: noncanonical list offset"));
                }
                spans.push(begin..end);
                previous = begin;
            }
        }
        spans
            .into_iter()
            .map(|span| self.decode_at(ty, &bytes[span], n + 1))
            .collect()
    }
}

// ── Entry points ────────────────────────────────────────────────────────────
//
// `F` selects the profile: `Pssz` is the automatic one (the bytes of the
// typed `Pssz` codec for the type the IR describes), `Pssz8/16/32` the
// explicit ones.

pub fn encode<F: PsszFormat>(schema: &Schema, ty: &AnyType, value: &Value) -> PsszResult<Vec<u8>> {
    encode_with::<F>(schema, ty, value, Options::default())
}
/// [`encode`] with a handler registry and / or opaque nested bytes.
pub fn encode_with<F: PsszFormat>(
    schema: &Schema,
    ty: &AnyType,
    value: &Value,
    options: Options<'_>,
) -> PsszResult<Vec<u8>> {
    let codec = Codec::with::<F>(schema, options)?;
    codec.check(ty)?;
    let bytes = codec.encode_at(ty, value, 0)?;
    codec.check_top_cap(ty, bytes.len())?;
    Ok(bytes)
}
/// [`encode_with`], and whether a failure is a handler's (a violated
/// invariant or a handler a strict registry lacks) rather than a mismatch
/// between the value and the schema.
pub(crate) fn encode_checked(
    schema: &Schema,
    ty: &AnyType,
    value: &Value,
    handlers: &CustomHandlers,
) -> Result<Vec<u8>, (PsszError, bool)> {
    let codec = Codec::with::<Pssz>(schema, Options::handlers(handlers)).map_err(|e| (e, false))?;
    codec.encode_at(ty, value, 0).map_err(|e| (e, codec.check_failed.get()))
}
/// Rejects unknown appended optional fields.
pub fn decode_exact<F: PsszFormat>(
    schema: &Schema,
    ty: &AnyType,
    bytes: &[u8],
) -> PsszResult<Value> {
    decode_exact_with::<F>(schema, ty, bytes, Options::default())
}
/// [`decode_exact`] with a handler registry and / or opaque nested bytes.
pub fn decode_exact_with<F: PsszFormat>(
    schema: &Schema,
    ty: &AnyType,
    bytes: &[u8],
    options: Options<'_>,
) -> PsszResult<Value> {
    let codec = Codec::with::<F>(schema, options)?;
    codec.check(ty)?;
    codec.check_top_cap(ty, bytes.len())?;
    let session = checked::DecodeSession::enter(false);
    let value = codec.decode_at(ty, bytes, 0)?;
    if session.has_unknown() {
        return Err(failure("pssz dynamic: unknown fields in exact decode"));
    }
    Ok(value)
}
/// Accepts unknown appended optional fields and reports them.
pub fn decode_compatible<F: PsszFormat>(
    schema: &Schema,
    ty: &AnyType,
    bytes: &[u8],
) -> PsszResult<Decoded<Value>> {
    decode_compatible_with::<F>(schema, ty, bytes, Options::default())
}
/// [`decode_compatible`] with a handler registry and / or opaque nested
/// bytes.
pub fn decode_compatible_with<F: PsszFormat>(
    schema: &Schema,
    ty: &AnyType,
    bytes: &[u8],
    options: Options<'_>,
) -> PsszResult<Decoded<Value>> {
    let codec = Codec::with::<F>(schema, options)?;
    codec.check(ty)?;
    codec.check_top_cap(ty, bytes.len())?;
    let session = checked::DecodeSession::enter(true);
    let value = codec.decode_at(ty, bytes, 0)?;
    Ok(Decoded {
        value,
        has_unknown: session.has_unknown(),
    })
}

/// Checks `bytes` against the schema under the automatic profile without
/// keeping the value (C++ `validate_auto`). Unknown appended optional fields
/// are accepted; the result tells whether there were any.
pub fn validate(schema: &Schema, ty: &AnyType, bytes: &[u8]) -> PsszResult<bool> {
    validate_with(schema, ty, bytes, Options::default())
}
/// [`validate`] with a handler registry (the invariants of the custom types
/// are checked, and a strict registry requires every handler) and / or
/// opaque nested bytes (the bytes of a Nested node are not examined).
pub fn validate_with(
    schema: &Schema,
    ty: &AnyType,
    bytes: &[u8],
    options: Options<'_>,
) -> PsszResult<bool> {
    decode_compatible_with::<Pssz>(schema, ty, bytes, options).map(|decoded| decoded.has_unknown)
}

/// The `key` encoding (docs/key-spec.md) of `value`, a dynamic value of
/// `ty`: the bytes the typed `psio::key` writes for the type the schema
/// describes. The value is first checked against the schema (integer ranges,
/// bounds, UTF-8, map / set uniqueness, and with a registry the invariants);
/// the entries of a map / set are written in canonical order whatever order
/// `value` holds them in.
///
/// The key of a Nested node is the key of the byte list holding its inner
/// value's canonical encoding; with `NestedBytes::Opaque` the value holds
/// those bytes and they are written as they are. A type with its own
/// `KeyPack` implementation is described by its wire type in the schema: its
/// key here is the wire type's.
pub fn to_key(schema: &Schema, ty: &AnyType, value: &Value) -> PsszResult<Vec<u8>> {
    to_key_with(schema, ty, value, Options::default())
}
/// [`to_key`] with a handler registry and / or opaque nested bytes.
pub fn to_key_with(
    schema: &Schema,
    ty: &AnyType,
    value: &Value,
    options: Options<'_>,
) -> PsszResult<Vec<u8>> {
    let codec = Codec::with::<Pssz>(schema, options)?;
    codec.check(ty)?;
    codec.encode_at(ty, value, 0)?;
    let mut out = Vec::new();
    codec.key_at(ty, value, &mut out, 0)?;
    Ok(out)
}
/// The canonical order of a map / set value's entries (indices into
/// `items`), for the text codec.
pub(crate) fn assoc_order(schema: &Schema, custom: &Custom, items: &[Value]) -> PsszResult<Vec<usize>> {
    let codec = Codec::new::<Pssz>(schema)?;
    codec.assoc_order(&assoc_shape(schema, custom)?, items)
}

/// The offset width the automatic profile derives for `ty`'s own header,
/// slots or offset table: what `psio::pssz::auto_pssz_width::<T>()` is for
/// the typed codec.
pub fn auto_width(schema: &Schema, ty: &AnyType) -> PsszResult<usize> {
    let codec = Codec::new::<Pssz>(schema)?;
    codec.check(ty)?;
    // A box has no header of its own: the boxed type is encoded at its own
    // width. As a root, the typed trait reports the unbounded width.
    let (wire, boxed) = schema.wire_boxed(ty)?;
    if boxed {
        return Ok(4);
    }
    codec.width_of(wire, None)
}
