//! JSON / I-JSON text for a type known only by its Schema IR (C++
//! `psio/pssz_ir_json.hpp`), and the conversions a schema-only consumer
//! needs:
//!
//! ```text
//! json_to_pssz::<IJson>(&schema, &root, text)    text  -> PSSZ
//! pssz_to_json::<IJson>(&schema, &root, bytes)   PSSZ  -> text
//! validate_pssz(&schema, &root, bytes)           check PSSZ
//! ```
//!
//! `root` is an `AnyType`; a named type is `AnyType::named("Name")`. The
//! PSSZ side is the automatic profile of `psio::pssz::dynamic` (the bytes of
//! the typed `Pssz` codec); the text is the text of the typed `Json` /
//! `IJson` codec (`psio::json`) for the type the IR describes:
//!
//! * record: object, members by name; unknown keys are skipped, a missing
//!   member must be an Option; a repeated key is read again and the last one
//!   wins
//! * tuple: array of exactly its arity
//! * variant: IJson `{"Name": value}` (Name: the member's `ijsonName`
//!   attribute, else its name); Json `[index, value]`
//! * option: null or the value
//! * list / array: array (exact count for an array, at most N bounded)
//! * string / utf8 adapter: JSON string (UTF-8; IJson: no noncharacters). A
//!   raw `string` holding bytes that are not UTF-8 has no text: an error
//! * box: the inner type's text
//! * any other Custom id (a custom type): through its handler in a
//!   [`CustomHandlers`] (the `*_with` entry points) one JSON string when
//!   the handler has a text form; with no handler, or one that only checks
//!   (an invariant), the inner type's text; an error when the registry is
//!   strict and has no handler
//! * Nested: the inner value's own text; reading stores its canonical
//!   encoding
//! * set: an array of its elements. map: an object when the key type is a
//!   `string` / `utf8` node or a custom type whose registered handler has a
//!   text form, an array of `[key, value]` pairs otherwise. Readers accept
//!   any order and reject a repeated key / element; writers emit the
//!   canonical (key byte) order
//! * hex adapter: IJson one hex string (UPPERCASE on output); Json an array
//!   of numbers
//! * Int: number; a 64-bit integer under IJson is a bare number while
//!   |v| <= 2^53 - 1, a decimal string beyond (either form is read, bare
//!   only in that range); a member's `jsonInt` / `ijsonInt` attribute forces
//!   one form and reaches the integer through option, list and array
//! * Int of 128 bits and unsigned Int of 256 bits: decimal; a bare number
//!   under Json, always a string under IJson. Read from a decimal string,
//!   the legacy "0x<hex>" string, or a bare number (IJson: |v| <= 2^53 - 1)
//! * Enum: a listed value is its name (the first listed, when two share a
//!   value), any other its number; read from the name, the number, the
//!   numeric string or "name:number" (which must agree). A number that
//!   names no enumerator is accepted: there is no typed enum to refuse it
//! * bool, Float: true / false; shortest round-trip decimal
//!
//! No JSON form (an error, not a guess): the bitvector / bitlist adapters,
//! and integers of any other width. A non-finite float has no JSON text:
//! writing one is an error in both formats, and none is read.
//!
//! The reader is a type walk over a [`JsonCursor`]: it holds no text
//! positions and does no tokenizing, so another parser can be put under it.
//! Everything is checked: errors, never panics; nesting and schema recursion
//! are limited by `MAX_DEPTH`.
use super::{
    assoc_shape, metadata, nested_member, parse_length_bound, Options, Value, MAP_DUPLICATE,
    NO_HANDLER, SET_DUPLICATE,
};
use crate::json::{
    write_variant_close, write_variant_open, IntForm, JsonCursor, JsonFormat, JsonKind,
    JsonText, JsonTextError, TextCursor, Writer, MAX_DEPTH,
};
use crate::json::{wide_is_safe, wide_parse, wide_to_i128, wide_to_u128, wide_to_u256, TextForm};
use crate::pssz::{Pssz, PsszError, PsszPack, PsszUnpack, Uint256};
use crate::pssz_schema_ir::{
    wire_custom_id, AnyType, AnyValue, Attribute, Custom, Enum, Int, Member, Schema,
    ToPsszSchema, Variant,
};
use std::collections::HashMap;

/// Why a schema-driven conversion failed: the schema or the PSSZ bytes
/// ([`PsszError`]), or the text ([`JsonTextError`]).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SchemaJsonError {
    Pssz(PsszError),
    Json(JsonTextError),
}
impl SchemaJsonError {
    /// The description, without position.
    pub fn what(&self) -> &str {
        match self {
            Self::Pssz(e) => &e.0,
            Self::Json(e) => &e.what,
        }
    }
}
impl std::fmt::Display for SchemaJsonError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Pssz(e) => write!(f, "{}", e.0),
            Self::Json(e) => e.fmt(f),
        }
    }
}
impl std::error::Error for SchemaJsonError {}
impl From<PsszError> for SchemaJsonError {
    fn from(e: PsszError) -> Self {
        Self::Pssz(e)
    }
}
impl From<JsonTextError> for SchemaJsonError {
    fn from(e: JsonTextError) -> Self {
        Self::Json(e)
    }
}

type Text<T> = Result<T, JsonTextError>;

const SAFE_INTEGER: u128 = (1 << 53) - 1;

fn find<'a>(attributes: &'a [Attribute], name: &str) -> Option<&'a str> {
    attributes.iter().find(|a| a.name == name)?.value.as_deref()
}
fn member_form(attributes: &[Attribute], ijson: bool) -> IntForm {
    match find(attributes, if ijson { "ijsonInt" } else { "jsonInt" }) {
        Some("string") => IntForm::String,
        Some(_) => IntForm::Number,
        None => IntForm::Natural,
    }
}
fn variant_name(member: &Member) -> &str {
    find(&member.attributes, "ijsonName").unwrap_or(&member.name)
}

/// The byte container under a string / hex adapter.
struct ByteShape {
    /// Array: exactly `limit` bytes.
    exact: bool,
    /// BoundedList: at most.
    limit: Option<u64>,
}
impl ByteShape {
    fn of(schema: &Schema, custom: &Custom) -> Option<Self> {
        match &schema.resolve(&custom.r#type).ok()?.value {
            AnyValue::List(_) => Some(Self { exact: false, limit: None }),
            AnyValue::BoundedList(l) => Some(Self { exact: false, limit: Some(l.maxCount) }),
            AnyValue::Array(a) => Some(Self { exact: true, limit: Some(a.len) }),
            _ => None,
        }
    }
    fn admits(&self, n: usize) -> bool {
        self.limit.is_none_or(|limit| if self.exact { n as u64 == limit } else { n as u64 <= limit })
    }
}

const BYTE: Int = Int { bits: 8, isSigned: false };

const ENUM_UNDERLYING: &str = "pssz dynamic: an enum needs an underlying integer of 8 to 64 bits";

/// The value of an integer of at most 64 bits as 64 two's-complement bits
/// (sign-extended), or `None` when `value` is not a value of `ty`.
fn integer_bits(ty: &Int, value: &Value) -> Option<u64> {
    match (ty.isSigned, value) {
        (false, Value::Unsigned(v)) if ty.bits <= 64 => {
            (*v <= (1u128 << ty.bits) - 1).then_some(*v as u64)
        }
        (true, Value::Signed(v)) if (1..=64).contains(&ty.bits) => {
            let half = 1i128 << (ty.bits - 1);
            (*v >= -half && *v < half).then_some(*v as i64 as u64)
        }
        _ => None,
    }
}

/// `0 | -?[1-9][0-9]*` within the range of an integer of `bits` (<= 64)
/// bits: (negative, magnitude).
fn parse_integer(text: &str, signed: bool, bits: u32) -> Option<(bool, u128)> {
    let (negative, digits) = match text.strip_prefix('-') {
        Some(rest) => (true, rest),
        None => (false, text),
    };
    if (negative && !signed) || digits.is_empty() || digits.len() > 20 {
        return None;
    }
    if digits.starts_with('0') && (digits.len() > 1 || negative) {
        return None;
    }
    let mut magnitude = 0u128;
    for c in digits.bytes() {
        if !c.is_ascii_digit() {
            return None;
        }
        magnitude = magnitude * 10 + u128::from(c - b'0');
    }
    let in_range = if !signed {
        magnitude <= (1u128 << bits) - 1
    } else {
        let half = 1u128 << (bits - 1);
        if negative { magnitude <= half } else { magnitude <= half - 1 }
    };
    in_range.then_some((negative, magnitude))
}

fn hex_value(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

/// How the key type of a map appears in text.
enum MapKey<'a> {
    /// Not a string by type: the map is an array of `[k, v]` pairs.
    Pairs,
    /// A `string` / `utf8` node.
    String(&'a Custom),
    /// A custom type whose registered handler has a text form.
    Handler(&'a Custom, &'a CustomHandler),
}
/// A handler that presents the key type decides the key's text. One that
/// only checks it (an invariant, no text form) leaves the text to the wire
/// type, like no handler at all.
fn map_key_text<'a>(
    schema: &'a Schema,
    key: &'a AnyType,
    handlers: Option<&'a CustomHandlers>,
) -> Result<MapKey<'a>, &'static str> {
    let mut at = key;
    for _ in 0..=4 * schema.types.len() + 64 {
        let Ok(resolved) = schema.resolve(at) else { return Ok(MapKey::Pairs) };
        let AnyValue::Custom(custom) = &resolved.value else { return Ok(MapKey::Pairs) };
        if matches!(custom.id.as_str(), "string" | "utf8") {
            return Ok(MapKey::String(custom));
        }
        if wire_custom_id(&custom.id) {
            return Ok(MapKey::Pairs);
        }
        if custom.id != "box" {
            // A time value is not an object key (docs/json.md).
            if crate::time::parse_time_schema_id(&custom.id).is_some()
                || crate::time::reserved_time_schema_id(&custom.id)
            {
                return Ok(MapKey::Pairs);
            }
            match handlers.and_then(|all| all.find(&custom.id)) {
                Some(handler) if handler.to_text.is_some() || handler.from_text.is_some() => {
                    return Ok(MapKey::Handler(custom, handler));
                }
                Some(_) => (),
                None if handlers.is_some_and(|all| all.strict) => return Err(NO_HANDLER),
                None => (),
            }
        }
        at = &custom.r#type;
    }
    Ok(MapKey::Pairs)
}

// ── Reading: the type walk over a cursor ────────────────────────────────────

struct Reader<'a, C: JsonCursor> {
    schema: &'a Schema,
    c: &'a mut C,
    ijson: bool,
    handlers: Option<&'a CustomHandlers>,
}

impl<C: JsonCursor> Reader<'_, C> {
    fn fail<T>(&self, what: impl Into<std::borrow::Cow<'static, str>>) -> Text<T> {
        Err(self.c.error(what))
    }
    fn resolve<'s>(&self, schema: &'s Schema, ty: &'s AnyType) -> Text<&'s AnyType> {
        schema.resolve(ty).map_err(|_| self.c.error("pssz dynamic: unresolved named type"))
    }

    /// A 128- or 256-bit integer: a decimal string (or the legacy "0x"
    /// string), or a bare decimal number — any in-range value under Json, at
    /// most 2^53 - 1 in magnitude under IJson. A forced form accepts that
    /// form only.
    fn wide_integer(&mut self, ty: &Int, form: IntForm) -> Text<Value> {
        if ty.bits != 128 && !(ty.bits == 256 && !ty.isSigned) {
            return self.fail("json: this integer width has no JSON form");
        }
        let kind = self.c.kind()?;
        let quoted = kind == JsonKind::String;
        let parsed = if quoted {
            if form == IntForm::Number {
                return self.fail("json: expected a number, found a string");
            }
            let (text, escaped) = self.c.string()?;
            if escaped { None } else { wide_parse(&text, ty.bits) }
        } else {
            if form == IntForm::String {
                return self.fail("json: expected an integer string");
            }
            if kind != JsonKind::Number {
                return self.fail("json: expected an integer");
            }
            wide_parse(self.c.number()?, ty.bits)
        };
        let value = parsed.and_then(|(negative, magnitude)| {
            let value = if ty.bits == 256 {
                let limbs = wide_to_u256(negative, magnitude)?.limb;
                Value::Bytes(limbs.iter().flat_map(|limb| limb.to_le_bytes()).collect())
            } else if ty.isSigned {
                Value::Signed(wide_to_i128(negative, magnitude)?)
            } else {
                Value::Unsigned(wide_to_u128(negative, magnitude)?)
            };
            Some((value, magnitude))
        });
        let Some((value, magnitude)) = value else {
            return self.fail(if quoted {
                "json: integer string is not a canonical in-range decimal"
            } else {
                "json: integer is not a canonical in-range decimal"
            });
        };
        // Natural IJson: a bare number only where JavaScript is exact.
        if !quoted && self.ijson && form == IntForm::Natural && !wide_is_safe(&magnitude) {
            return self.fail("ijson: bare integer outside the safe range must be a string");
        }
        Ok(value)
    }

    /// An Enum: the enumerator's name ("active"), the number (2 or the
    /// numeric string "2"), or both ("active:2", which must agree). An
    /// unknown name is an error; a number that names no enumerator is
    /// accepted.
    fn enumeration(&mut self, ty: &Enum, form: IntForm) -> Text<Value> {
        let schema = self.schema;
        let int = match &self.resolve(schema, &ty.r#type)?.value {
            AnyValue::Int(int) if matches!(int.bits, 8 | 16 | 32 | 64) => int,
            _ => return self.fail(ENUM_UNDERLYING),
        };
        if self.c.kind()? != JsonKind::String {
            return self.integer(int, form);
        }
        let text = self.c.string()?.0.into_owned();
        const BAD_NUMBER: &str = "json: enum number is not a canonical in-range decimal";
        let (negative, magnitude);
        if text.bytes().next().is_some_and(|c| c == b'-' || c.is_ascii_digit()) {
            match parse_integer(&text, int.isSigned, int.bits) {
                Some(parsed) => (negative, magnitude) = parsed,
                None => return self.fail(BAD_NUMBER),
            }
        } else {
            let (name, stated) = match text.split_once(':') {
                Some((name, stated)) => (name, Some(stated)),
                None => (text.as_str(), None),
            };
            let Some(found) = ty.members.iter().find(|member| member.name == name) else {
                return self.fail("json: unknown enumerator name");
            };
            match stated {
                Some(stated) => {
                    match parse_integer(stated, int.isSigned, int.bits) {
                        Some(parsed) => (negative, magnitude) = parsed,
                        None => return self.fail(BAD_NUMBER),
                    }
                    let low = magnitude as u64;
                    let bits = if negative { 0u64.wrapping_sub(low) } else { low };
                    if bits != found.value {
                        return self.fail("json: enumerator name and number disagree");
                    }
                }
                None => {
                    let raw = found.value as i64;
                    negative = int.isSigned && raw < 0;
                    magnitude = if negative {
                        u128::from(raw.unsigned_abs())
                    } else {
                        u128::from(found.value)
                    };
                }
            }
        }
        Ok(if int.isSigned {
            Value::Signed(if negative { -(magnitude as i128) } else { magnitude as i128 })
        } else {
            Value::Unsigned(magnitude)
        })
    }

    fn integer(&mut self, ty: &Int, form: IntForm) -> Text<Value> {
        if ty.bits > 64 {
            return self.wide_integer(ty, form);
        }
        if !matches!(ty.bits, 1 | 8 | 16 | 32 | 64) {
            return self.fail("json: this integer width has no JSON form");
        }
        let wide = self.ijson && ty.bits == 64 && form == IntForm::Natural;
        let kind = self.c.kind()?;
        let parsed = if kind == JsonKind::String {
            if form != IntForm::String && !wide {
                return self.fail("json: expected a number, found a string");
            }
            let (text, escaped) = self.c.string()?;
            let parsed = if escaped { None } else { parse_integer(&text, ty.isSigned, ty.bits) };
            match parsed {
                Some(parsed) => parsed,
                None => {
                    return self.fail("json: integer string is not a canonical in-range decimal")
                }
            }
        } else {
            if form == IntForm::String {
                return self.fail("json: expected an integer string");
            }
            if kind != JsonKind::Number {
                return self.fail("json: expected an integer");
            }
            let parsed = parse_integer(self.c.number()?, ty.isSigned, ty.bits);
            let Some(parsed) = parsed else {
                return self.fail("json: integer is not a canonical in-range decimal");
            };
            // Natural IJson: a bare number only where JavaScript is exact.
            if wide && parsed.1 > SAFE_INTEGER {
                return self.fail("ijson: bare integer outside the safe range must be a string");
            }
            parsed
        };
        let (negative, magnitude) = parsed;
        Ok(if ty.isSigned {
            // magnitude <= 2^63, checked by parse_integer.
            Value::Signed(if negative { -(magnitude as i128) } else { magnitude as i128 })
        } else {
            Value::Unsigned(magnitude)
        })
    }

    fn array_items(
        &mut self,
        limit: Option<u64>,
        exact: bool,
        mut item: impl FnMut(&mut Self) -> Text<()>,
    ) -> Text<()> {
        self.c.begin_array()?;
        let mut count = 0u64;
        while self.c.next_element()? {
            if limit.is_some_and(|limit| count >= limit) {
                return self.fail(if exact {
                    "json: array has too many elements"
                } else {
                    "json: array exceeds its bound"
                });
            }
            count += 1;
            item(self)?;
        }
        if exact && limit.is_some_and(|limit| count != limit) {
            return self.fail("json: array has too few elements");
        }
        Ok(())
    }

    fn sequence(
        &mut self,
        element: &AnyType,
        limit: Option<u64>,
        exact: bool,
        form: IntForm,
    ) -> Text<Value> {
        let mut items = Vec::new();
        self.array_items(limit, exact, |this| {
            items.push(this.read(element, form, 0)?);
            Ok(())
        })?;
        Ok(Value::Sequence(items))
    }

    /// A Custom id that is not part of the binary form. A `box` reads as
    /// its inner type. A text presentation reads through its handler: one
    /// JSON string, turned into the PSSZ bytes of the inner type, which are
    /// then read as a value of it; the plain form is then not accepted.
    /// With no handler it reads as the inner type, unless the registry is
    /// strict.
    fn transparent(&mut self, ty: &Custom, form: IntForm, n: usize) -> Text<Value> {
        if ty.id != "box" {
            if let Some(kind) = crate::time::parse_time_schema_id(&ty.id) {
                return self.time(ty, kind, form);
            }
            if crate::time::reserved_time_schema_id(&ty.id) {
                return self.fail("pssz dynamic: unsupported custom adapter");
            }
            let handler = self.handlers.and_then(|all| all.find(&ty.id));
            if handler.is_none() && self.handlers.is_some_and(|all| all.strict) {
                return self.fail(NO_HANDLER);
            }
            // A handler with only a `check` leaves the wire type's text.
            if let Some(from_text) = handler.and_then(|handler| handler.from_text.as_ref()) {
                if self.c.kind()? != JsonKind::String {
                    return self.fail("json: expected a string (custom text form)");
                }
                let text = self.c.string()?.0.into_owned();
                let Ok(bytes) = from_text(&text) else {
                    return self.fail("json: text is not a value of the custom type");
                };
                return match super::decode_compatible::<Pssz>(self.schema, &ty.r#type, &bytes) {
                    Ok(decoded) if !decoded.has_unknown => Ok(decoded.value),
                    _ => self
                        .fail("json: custom handler produced bytes that are not the inner type"),
                };
            }
        }
        self.read(&ty.r#type, form, n + 1)
    }

    /// The built-in presentation of the time ids (docs/time.md): a duration
    /// or a monotonic time point is its tick integer; a time point is
    /// RFC 3339 text.
    fn time(&mut self, ty: &Custom, kind: crate::time::TimeKind, form: IntForm) -> Text<Value> {
        let ticks = match self.schema.time_ticks(&ty.r#type) {
            Ok(ticks) => Int { bits: ticks.bits, isSigned: ticks.isSigned },
            Err(error) => return self.fail(error.0),
        };
        if !kind.point {
            return self.integer(&ticks, form);
        }
        let Some(digits) = crate::time::time_fraction_digits(kind.num, kind.den) else {
            return self.fail("json: a time point of this unit has no text form");
        };
        if self.c.kind()? != JsonKind::String {
            return self.fail("json: expected a string (RFC 3339 time point)");
        }
        let text = self.c.string()?.0.into_owned();
        match crate::time::time_ticks_of_text(&text, digits, ticks.bits, ticks.isSigned) {
            Ok(value) if ticks.isSigned => Ok(Value::Signed(value)),
            Ok(value) => Ok(Value::Unsigned(value as u128)),
            Err(error) => self.fail(error.0),
        }
    }

    fn custom(&mut self, ty: &Custom, form: IntForm, n: usize) -> Text<Value> {
        match ty.id.as_str() {
            "bool" => return Ok(Value::Bool(self.c.boolean()?)),
            "char" => return self.integer(&BYTE, form),
            "bitvector" | "bitlist" => return self.fail("json: bit types have no JSON form"),
            "string" | "utf8" | "hex" => (),
            "map" | "set" => return self.assoc(ty, form),
            "unknown" => return self.fail("pssz dynamic: unsupported custom adapter"),
            _ => return self.transparent(ty, form, n),
        }
        let Some(shape) = ByteShape::of(self.schema, ty) else {
            return self.fail("pssz dynamic: byte adapter requires byte container");
        };
        if ty.id != "hex" {
            if self.c.kind()? != JsonKind::String {
                return self.fail("json: expected a string");
            }
            let (text, _) = self.c.string()?;
            if !shape.admits(text.len()) {
                return self.fail("json: string exceeds its bound");
            }
            return Ok(Value::String(text.into_owned()));
        }
        let mut bytes = Vec::new();
        if self.ijson {
            if self.c.kind()? != JsonKind::String {
                return self.fail("ijson: expected a hex string");
            }
            let (text, escaped) = self.c.string()?;
            let digits = text.as_bytes();
            let problem = if escaped || digits.iter().any(|d| hex_value(*d).is_none()) {
                Some("ijson: non-hex character in hex string")
            } else if digits.len() % 2 != 0 {
                Some("ijson: odd-length hex string")
            } else if !shape.admits(digits.len() / 2) {
                Some(if shape.exact {
                    "ijson: hex string has the wrong length"
                } else {
                    "json: array exceeds its bound"
                })
            } else {
                bytes.reserve(digits.len() / 2);
                for pair in digits.chunks_exact(2) {
                    // Both digits were checked above.
                    bytes.push((hex_value(pair[0]).unwrap_or(0) << 4) | hex_value(pair[1]).unwrap_or(0));
                }
                None
            };
            if let Some(problem) = problem {
                return self.fail(problem);
            }
        } else {
            self.array_items(shape.limit, shape.exact, |this| {
                if let Value::Unsigned(byte) = this.integer(&BYTE, form)? {
                    bytes.push(byte as u8);
                }
                Ok(())
            })?;
        }
        Ok(Value::Bytes(bytes))
    }

    /// An object member name as a key value: what `read` makes of the same
    /// text as a JSON string of the key type.
    fn key_from_text(&mut self, key: &MapKey<'_>, text: &str) -> Text<Value> {
        match key {
            MapKey::Pairs => self.fail("json: expected an array of [key, value] pairs"),
            MapKey::String(node) => {
                let Some(shape) = ByteShape::of(self.schema, node) else {
                    return self.fail("pssz dynamic: byte adapter requires byte container");
                };
                if !shape.admits(text.len()) {
                    return self.fail("json: string exceeds its bound");
                }
                Ok(Value::String(text.to_owned()))
            }
            MapKey::Handler(node, handler) => {
                let bytes = match handler.from_text.as_ref().map(|from_text| from_text(text)) {
                    Some(Ok(bytes)) => bytes,
                    _ => return self.fail("json: text is not a value of the custom type"),
                };
                match super::decode_compatible::<Pssz>(self.schema, &node.r#type, &bytes) {
                    Ok(decoded) if !decoded.has_unknown => Ok(decoded.value),
                    _ => self
                        .fail("json: custom handler produced bytes that are not the inner type"),
                }
            }
        }
    }

    /// A map or a set: any order is read, a repeated key / element is an
    /// error, and the value is in canonical order.
    fn assoc(&mut self, ty: &Custom, form: IntForm) -> Text<Value> {
        let schema = self.schema;
        let shape = match assoc_shape(schema, ty) {
            Ok(shape) => shape,
            Err(error) => return self.fail(error.0),
        };
        let mut items = Vec::new();
        let key = if shape.map {
            match map_key_text(schema, shape.key, self.handlers) {
                Ok(key) => key,
                Err(what) => return self.fail(what),
            }
        } else {
            MapKey::Pairs
        };
        if let MapKey::Pairs = key {
            // A set element, or a map entry as the [k, v] tuple it is.
            let form = if shape.map { IntForm::Natural } else { form };
            self.array_items(shape.max, false, |this| {
                items.push(this.read(shape.element, form, 0)?);
                Ok(())
            })?;
        } else {
            let Some(value_type) = shape.value else {
                return self.fail("pssz dynamic: a map entry is a [key, value] pair");
            };
            if self.c.kind()? != JsonKind::Object {
                return self.fail("json: expected an object (a map with string keys)");
            }
            self.c.begin_object()?;
            loop {
                let name = match self.c.next_field()? {
                    None => break,
                    Some(name) => name.into_owned(),
                };
                if shape.max.is_some_and(|max| items.len() as u64 >= max) {
                    return self.fail("json: array exceeds its bound");
                }
                let key = self.key_from_text(&key, &name)?;
                let value = self.read(value_type, IntForm::Natural, 0)?;
                items.push(Value::Sequence(vec![key, value]));
            }
        }
        let order = match super::assoc_order(schema, ty, &items) {
            Ok(order) => order,
            Err(error) if error.0 == MAP_DUPLICATE => return self.fail("json: duplicate map key"),
            Err(error) if error.0 == SET_DUPLICATE => {
                return self.fail("json: duplicate set element")
            }
            Err(error) => return self.fail(error.0),
        };
        let mut slots: Vec<Option<Value>> = items.into_iter().map(Some).collect();
        Ok(Value::Sequence(order.into_iter().filter_map(|index| slots[index].take()).collect()))
    }

    fn record(&mut self, members: &[Member]) -> Text<Value> {
        self.c.begin_object()?;
        // One slot per member, filled as keys arrive; a repeated key
        // overwrites its slot (last one wins).
        let mut slots: Vec<Option<Value>> = members.iter().map(|_| None).collect();
        loop {
            let index = match self.c.next_field()? {
                None => break,
                Some(key) => members.iter().position(|m| m.name == *key),
            };
            let Some(index) = index else {
                self.c.skip()?;
                continue;
            };
            let member = &members[index];
            let value = self.read(&member.r#type, member_form(&member.attributes, self.ijson), 0)?;
            let bound = match parse_length_bound(&member.attributes) {
                Ok(bound) => bound,
                Err(_) => return self.fail("pssz dynamic: malformed lengthBound attribute"),
            };
            // A Nested member's bound counts encoded bytes: it is checked
            // where the bytes exist (the binary codec).
            let nested = bound.is_some()
                && nested_member(self.schema, &member.r#type).unwrap_or(false);
            if !nested && bound.is_some_and(|bound| !bound.admits_value(&value)) {
                return self.fail("json: member length_bound violated");
            }
            slots[index] = Some(value);
        }
        let mut values = Vec::with_capacity(members.len());
        for (member, slot) in members.iter().zip(slots) {
            let value = match slot {
                Some(value) => value,
                // A missing Option member is empty; any other is an error.
                None => match self.resolve(self.schema, &member.r#type)?.value {
                    AnyValue::Option(_) => Value::Optional(None),
                    _ => return self.fail("json: missing required member"),
                },
            };
            values.push((member.name.clone(), value));
        }
        Ok(Value::Record(values))
    }

    fn variant(&mut self, ty: &Variant) -> Text<Value> {
        let (index, payload);
        if self.ijson {
            if self.c.kind()? != JsonKind::Object {
                return self.fail("ijson: expected a variant object");
            }
            self.c.begin_object()?;
            let found = match self.c.next_field()? {
                None => return self.fail("ijson: variant object must have one key"),
                Some(key) => ty.members.iter().position(|m| *variant_name(m) == *key),
            };
            let Some(found) = found else {
                return self.fail("ijson: unknown variant alternative");
            };
            index = found;
            payload = self.read(&ty.members[index].r#type, IntForm::Natural, 0)?;
            if self.c.next_field()?.is_some() {
                return self.fail("ijson: variant object must have one key");
            }
        } else {
            if self.c.kind()? != JsonKind::Array {
                return self.fail("json: expected a [index, value] variant");
            }
            self.c.begin_array()?;
            if !self.c.next_element()? {
                return self.fail("json: expected a [index, value] variant");
            }
            let wanted = match self.integer(&Int { bits: 32, isSigned: false }, IntForm::Natural)? {
                Value::Unsigned(wanted) => wanted,
                _ => u128::MAX,
            };
            if wanted >= ty.members.len() as u128 {
                return self.fail("json: variant index out of range");
            }
            index = wanted as usize;
            if !self.c.next_element()? {
                return self.fail("json: expected ',' in variant");
            }
            payload = self.read(&ty.members[index].r#type, IntForm::Natural, 0)?;
            if self.c.next_element()? {
                return self.fail("json: expected ']' after variant value");
            }
        }
        Ok(Value::Variant { selector: index, value: Box::new(payload) })
    }

    /// `n` counts schema steps that consume no text (nested Options); it
    /// restarts inside every array / object, whose nesting the cursor
    /// limits. Together they bound the recursion for any schema and text.
    fn read(&mut self, input: &AnyType, form: IntForm, n: usize) -> Text<Value> {
        if n > MAX_DEPTH {
            return self.fail("json: max depth exceeded");
        }
        let schema = self.schema;
        let ty = self.resolve(schema, input)?;
        match &ty.value {
            AnyValue::Int(i) => self.integer(i, form),
            AnyValue::Enum(e) => self.enumeration(e, form),
            AnyValue::Float(f) => {
                let token = self.c.number()?;
                let value = if f.exp == 8 {
                    token.parse::<f32>().ok().filter(|v| v.is_finite()).map(Value::Float32)
                } else {
                    token.parse::<f64>().ok().filter(|v| v.is_finite()).map(Value::Float64)
                };
                match value {
                    Some(value) => Ok(value),
                    None => self.fail("json: number out of range"),
                }
            }
            AnyValue::Custom(c) => self.custom(c, form, n),
            AnyValue::Option(o) => {
                if self.c.kind()? == JsonKind::Null {
                    self.c.null()?;
                    return Ok(Value::Optional(None));
                }
                Ok(Value::Optional(Some(Box::new(self.read(&o.r#type, form, n + 1)?))))
            }
            AnyValue::List(l) => self.sequence(&l.r#type, None, false, form),
            AnyValue::BoundedList(l) => self.sequence(&l.r#type, Some(l.maxCount), false, form),
            AnyValue::Array(a) => self.sequence(&a.r#type, Some(a.len), true, form),
            AnyValue::Tuple(t) => {
                let mut items = Vec::new();
                self.array_items(Some(t.members.len() as u64), true, |this| {
                    // array_items admits at most `members.len()` elements.
                    let Some(member) = t.members.get(items.len()) else {
                        return this.fail("json: array has too many elements");
                    };
                    items.push(this.read(member, IntForm::Natural, 0)?);
                    Ok(())
                })?;
                Ok(Value::Sequence(items))
            }
            AnyValue::Object(o) => self.record(&o.members),
            AnyValue::Struct(s) => self.record(&s.members),
            AnyValue::Variant(v) => self.variant(v),
            // The inner value's own text.
            AnyValue::Nested(nested) => self.read(&nested.r#type, form, n + 1),
            _ => self.fail("pssz dynamic: unsupported schema type"),
        }
    }
}

// ── Writing ─────────────────────────────────────────────────────────────────
//
// On the typed writer (`psio::json::Writer`), so integers, floats, strings
// and hex are formatted by the same code as the typed codec.

/// A hand-built `Value` may nest further than any decoded one; the writer
/// follows the value, so it is bounded here.
const MAX_VALUE_DEPTH: usize = MAX_DEPTH * (MAX_DEPTH + 2);

struct ValueWriter<'a> {
    schema: &'a Schema,
    w: Writer,
    handlers: Option<&'a CustomHandlers>,
}

impl ValueWriter<'_> {
    fn fail<T>(&self, what: impl Into<std::borrow::Cow<'static, str>>) -> Text<T> {
        Err(self.w.err(what))
    }
    fn resolve<'s>(&self, schema: &'s Schema, ty: &'s AnyType) -> Text<&'s AnyType> {
        schema.resolve(ty).map_err(|_| self.w.err("pssz dynamic: unresolved named type"))
    }

    /// The form in force is the writer's (set per record member).
    fn integer(&mut self, ty: &Int, value: &Value) -> Text<()> {
        const MISMATCH: &str = "pssz dynamic: integer value does not match the schema";
        // 128 and 256 bits: decimal, a string under natural IJson.
        match (ty.bits, ty.isSigned, value) {
            (256, false, Value::Bytes(bytes)) => {
                let mut limb = [0u64; 4];
                if bytes.len() != 32 {
                    return self
                        .fail("pssz dynamic: 256-bit integer needs 32 little-endian bytes");
                }
                for (limb, chunk) in limb.iter_mut().zip(bytes.chunks_exact(8)) {
                    *limb = chunk.iter().rev().fold(0u64, |acc, byte| (acc << 8) | u64::from(*byte));
                }
                return Uint256 { limb }.write_json(&mut self.w);
            }
            (256, false, _) => {
                return self.fail("pssz dynamic: 256-bit integer needs 32 little-endian bytes")
            }
            (128, false, Value::Unsigned(v)) => return v.write_json(&mut self.w),
            (128, true, Value::Signed(v)) => return v.write_json(&mut self.w),
            (128, _, _) => return self.fail(MISMATCH),
            (bits, _, _) if bits > 64 => {
                return self.fail("json: this integer width has no JSON form")
            }
            _ => (),
        }
        match (ty.isSigned, value) {
            (false, Value::Unsigned(v)) => {
                if ty.bits == 64 {
                    match u64::try_from(*v) {
                        Ok(v) => v.write_json(&mut self.w),
                        Err(_) => self.fail(MISMATCH),
                    }
                } else if *v <= (1u128 << ty.bits) - 1 {
                    (*v as u32).write_json(&mut self.w)
                } else {
                    self.fail(MISMATCH)
                }
            }
            (true, Value::Signed(v)) => {
                let half = 1i128 << (ty.bits.max(1) - 1);
                if *v < -half || *v >= half {
                    self.fail(MISMATCH)
                } else if ty.bits == 64 {
                    (*v as i64).write_json(&mut self.w)
                } else {
                    (*v as i32).write_json(&mut self.w)
                }
            }
            _ => self.fail(MISMATCH),
        }
    }

    /// The built-in presentation of the time ids (docs/time.md).
    fn time(&mut self, ty: &Custom, kind: crate::time::TimeKind, value: &Value) -> Text<()> {
        const MISMATCH: &str = "pssz dynamic: integer value does not match the schema";
        let ticks = match self.schema.time_ticks(&ty.r#type) {
            Ok(ticks) => Int { bits: ticks.bits, isSigned: ticks.isSigned },
            Err(error) => return self.fail(error.0),
        };
        if !kind.point {
            return self.integer(&ticks, value);
        }
        let Some(digits) = crate::time::time_fraction_digits(kind.num, kind.den) else {
            return self.fail("json: a time point of this unit has no text form");
        };
        let count = match (ticks.isSigned, value) {
            (true, Value::Signed(v))
                if *v >= -(1i128 << (ticks.bits - 1)) && *v < (1i128 << (ticks.bits - 1)) =>
            {
                *v
            }
            (false, Value::Unsigned(v)) if *v < (1u128 << ticks.bits) => *v as i128,
            _ => return self.fail(MISMATCH),
        };
        match crate::time::time_text_of_ticks(count, digits) {
            Ok(text) => self.w.string(text.as_bytes()),
            Err(_) => self.fail("json: time point outside years 0000..9999 has no text"),
        }
    }

    fn custom(&mut self, ty: &Custom, value: &Value, n: usize) -> Text<()> {
        if !wire_custom_id(&ty.id) {
            if ty.id != "box" {
                if let Some(kind) = crate::time::parse_time_schema_id(&ty.id) {
                    return self.time(ty, kind, value);
                }
                if crate::time::reserved_time_schema_id(&ty.id) {
                    return self.fail("pssz dynamic: unsupported custom adapter");
                }
                let handler = self.handlers.and_then(|all| all.find(&ty.id));
                if handler.is_none() && self.handlers.is_some_and(|all| all.strict) {
                    return self.fail(NO_HANDLER);
                }
                // A handler with only a `check` leaves the wire type's text.
                if let Some(to_text) = handler.and_then(|handler| handler.to_text.as_ref()) {
                    // The inner value as PSSZ bytes, then the handler's
                    // text as one JSON string.
                    let Ok(bytes) = super::encode::<Pssz>(self.schema, &ty.r#type, value) else {
                        return self
                            .fail("pssz dynamic: custom value does not match its inner type");
                    };
                    let Ok(text) = to_text(&bytes) else {
                        return self.fail("json: custom handler has no text for the value");
                    };
                    return self.w.string(text.as_bytes());
                }
            }
            // A box, or a text presentation with no handler: the inner type.
            return self.write(&ty.r#type, value, n + 1);
        }
        match (ty.id.as_str(), value) {
            ("bool", Value::Bool(v)) => return v.write_json(&mut self.w),
            ("bool", _) => return self.fail("pssz dynamic: bool value required"),
            ("char", Value::Unsigned(v)) if *v <= 255 => {
                return (*v as u8).write_json(&mut self.w)
            }
            ("char", _) => return self.fail("pssz dynamic: char byte value required"),
            ("bitvector" | "bitlist", _) => {
                return self.fail("json: bit types have no JSON form")
            }
            ("string" | "utf8" | "hex", _) => (),
            ("map" | "set", _) => return self.assoc(ty, value, n),
            _ => return self.fail("pssz dynamic: unsupported custom adapter"),
        }
        let Some(shape) = ByteShape::of(self.schema, ty) else {
            return self.fail("pssz dynamic: byte adapter requires byte container");
        };
        if ty.id != "hex" {
            let text = match value {
                Value::String(text) => text.as_bytes(),
                // A raw string holding bytes that are not UTF-8 has no JSON
                // text: `Writer::string` reports it.
                Value::Bytes(bytes) if ty.id == "string" => bytes,
                _ => return self.fail("pssz dynamic: string value required"),
            };
            if !shape.admits(text.len()) {
                return self.fail("json: string exceeds its bound");
            }
            return self.w.string(text);
        }
        let Value::Bytes(bytes) = value else {
            return self.fail("pssz dynamic: bytes value required");
        };
        if !shape.admits(bytes.len()) {
            return self.fail("json: array exceeds its bound");
        }
        // Hex under IJson, an array of numbers under Json.
        u8::write_json_slice(bytes, &mut self.w)
    }

    /// A map or a set, in canonical order: a set as an array, a map as an
    /// object (a key type that is a string by type) or an array of pairs.
    fn assoc(&mut self, ty: &Custom, value: &Value, n: usize) -> Text<()> {
        let Value::Sequence(items) = value else {
            return self.fail("pssz dynamic: sequence value required");
        };
        let schema = self.schema;
        let shape = match assoc_shape(schema, ty) {
            Ok(shape) => shape,
            Err(error) => return self.fail(error.0),
        };
        if shape.max.is_some_and(|max| items.len() as u64 > max) {
            return self.fail("json: array exceeds its bound");
        }
        let order = match super::assoc_order(schema, ty, items) {
            Ok(order) => order,
            Err(error) if error.0 == MAP_DUPLICATE => {
                return self.fail("json: map has duplicate keys")
            }
            Err(error) if error.0 == SET_DUPLICATE => {
                return self.fail("json: set has duplicate elements")
            }
            Err(error) => return self.fail(error.0),
        };
        let key = if shape.map {
            match map_key_text(schema, shape.key, self.handlers) {
                Ok(key) => key,
                Err(what) => return self.fail(what),
            }
        } else {
            MapKey::Pairs
        };
        let object = !matches!(key, MapKey::Pairs);
        // An entry is a tuple: its key and value take the natural form. The
        // elements of a set take the member's.
        let saved = shape.map.then(|| self.w.member_form(IntForm::Natural, IntForm::Natural));
        self.w.raw(if object { "{" } else { "[" });
        for (i, index) in order.into_iter().enumerate() {
            if i > 0 {
                self.w.raw(",");
            }
            let item = &items[index];
            if !object {
                self.write(shape.element, item, n + 1)?;
                continue;
            }
            let (Value::Sequence(entry), Some(value_type)) = (item, shape.value) else {
                return self.fail("pssz dynamic: a map entry is a [key, value] pair");
            };
            match &key {
                MapKey::Pairs => (),
                MapKey::String(node) => {
                    let text = match &entry[0] {
                        Value::String(text) => text.as_bytes(),
                        Value::Bytes(bytes) if node.id == "string" => bytes,
                        _ => return self.fail("pssz dynamic: string value required"),
                    };
                    if !ByteShape::of(schema, node).is_some_and(|shape| shape.admits(text.len())) {
                        return self.fail("json: string exceeds its bound");
                    }
                    self.w.string(text)?;
                }
                MapKey::Handler(node, handler) => {
                    let Ok(bytes) = super::encode::<Pssz>(schema, &node.r#type, &entry[0]) else {
                        return self
                            .fail("pssz dynamic: custom value does not match its inner type");
                    };
                    let text = match handler.to_text.as_ref().map(|to_text| to_text(&bytes)) {
                        Some(Ok(text)) => text,
                        _ => return self.fail("json: custom handler has no text for the value"),
                    };
                    self.w.string(text.as_bytes())?;
                }
            }
            self.w.raw(":");
            self.write(value_type, &entry[1], n + 1)?;
        }
        self.w.raw(if object { "}" } else { "]" });
        if let Some(saved) = saved {
            self.w.restore_form(saved);
        }
        Ok(())
    }

    fn sequence(
        &mut self,
        element: &AnyType,
        value: &Value,
        limit: Option<u64>,
        exact: bool,
        n: usize,
    ) -> Text<()> {
        let Value::Sequence(items) = value else {
            return self.fail("pssz dynamic: sequence value required");
        };
        let count = items.len() as u64;
        if limit.is_some_and(|limit| if exact { count != limit } else { count > limit }) {
            return self.fail("json: array exceeds its bound");
        }
        self.w.raw("[");
        for (i, item) in items.iter().enumerate() {
            if i > 0 {
                self.w.raw(",");
            }
            self.write(element, item, n + 1)?;
        }
        self.w.raw("]");
        Ok(())
    }

    fn record(&mut self, members: &[Member], value: &Value, n: usize) -> Text<()> {
        let Value::Record(fields) = value else {
            return self.fail("pssz dynamic: ordered record value required");
        };
        let schema = self.schema;
        let absent = Value::Optional(None);
        self.w.raw("{");
        for (i, member) in members.iter().enumerate() {
            let field = fields.iter().rev().find(|(name, _)| *name == member.name);
            let member_type = self.resolve(schema, &member.r#type)?;
            let field = match field {
                Some((_, field)) => field,
                None if matches!(member_type.value, AnyValue::Option(_)) => &absent,
                None => return self.fail("pssz dynamic: missing required record value"),
            };
            let bound = match parse_length_bound(&member.attributes) {
                Ok(bound) => bound,
                Err(_) => return self.fail("pssz dynamic: malformed lengthBound attribute"),
            };
            let nested =
                bound.is_some() && nested_member(schema, &member.r#type).unwrap_or(false);
            if !nested && bound.is_some_and(|bound| !bound.admits_value(field)) {
                return self.fail("json: member length_bound violated");
            }
            self.w.key(&member.name, i == 0)?;
            let saved = self.w.member_form(
                member_form(&member.attributes, false),
                member_form(&member.attributes, true),
            );
            self.write(member_type, field, n + 1)?;
            self.w.restore_form(saved);
        }
        self.w.raw("}");
        Ok(())
    }

    fn write(&mut self, input: &AnyType, value: &Value, n: usize) -> Text<()> {
        if n > MAX_VALUE_DEPTH {
            return self.fail("json: max depth exceeded");
        }
        let schema = self.schema;
        let ty = self.resolve(schema, input)?;
        match &ty.value {
            AnyValue::Int(i) => self.integer(i, value),
            AnyValue::Enum(e) => {
                let int = match &self.resolve(schema, &e.r#type)?.value {
                    AnyValue::Int(int) if matches!(int.bits, 8 | 16 | 32 | 64) => int,
                    _ => return self.fail(ENUM_UNDERLYING),
                };
                let Some(bits) = integer_bits(int, value) else {
                    return self.fail("pssz dynamic: integer value does not match the schema");
                };
                // A listed value is its (first) name; any other its number.
                match e.members.iter().find(|member| member.value == bits) {
                    Some(member) => self.w.string(member.name.as_bytes()),
                    None => self.integer(int, value),
                }
            }
            AnyValue::Float(f) => match value {
                Value::Float32(v) if f.exp == 8 => v.write_json(&mut self.w),
                Value::Float64(v) if f.exp == 11 => v.write_json(&mut self.w),
                _ => self.fail("pssz dynamic: float value does not match the schema"),
            },
            AnyValue::Custom(c) => self.custom(c, value, n),
            AnyValue::Option(o) => match value {
                Value::Optional(None) => {
                    self.w.raw("null");
                    Ok(())
                }
                Value::Optional(Some(inner)) => self.write(&o.r#type, inner, n + 1),
                _ => self.fail("pssz dynamic: explicit optional value required"),
            },
            AnyValue::List(l) => self.sequence(&l.r#type, value, None, false, n),
            AnyValue::BoundedList(l) => {
                self.sequence(&l.r#type, value, Some(l.maxCount), false, n)
            }
            AnyValue::Array(a) => self.sequence(&a.r#type, value, Some(a.len), true, n),
            AnyValue::Tuple(t) => {
                let items = match value {
                    Value::Sequence(items) if items.len() == t.members.len() => items,
                    _ => return self.fail("pssz dynamic: tuple value count mismatch"),
                };
                let saved = self.w.member_form(IntForm::Natural, IntForm::Natural);
                self.w.raw("[");
                for (i, (member, item)) in t.members.iter().zip(items).enumerate() {
                    if i > 0 {
                        self.w.raw(",");
                    }
                    self.write(member, item, n + 1)?;
                }
                self.w.raw("]");
                self.w.restore_form(saved);
                Ok(())
            }
            AnyValue::Object(o) => self.record(&o.members, value, n),
            AnyValue::Struct(s) => self.record(&s.members, value, n),
            AnyValue::Variant(v) => {
                let (member, selector, payload) = match value {
                    Value::Variant { selector, value } => match v.members.get(*selector) {
                        Some(member) => (member, *selector, value),
                        None => return self.fail("pssz dynamic: invalid explicit variant"),
                    },
                    _ => return self.fail("pssz dynamic: invalid explicit variant"),
                };
                let Ok(index) = u32::try_from(selector) else {
                    return self.fail("pssz dynamic: invalid explicit variant");
                };
                let saved = write_variant_open(&mut self.w, variant_name(member), index)?;
                self.write(&member.r#type, payload, n + 1)?;
                write_variant_close(&mut self.w, saved);
                Ok(())
            }
            // The inner value's own text.
            AnyValue::Nested(nested) => self.write(&nested.r#type, value, n + 1),
            _ => self.fail("pssz dynamic: unsupported schema type"),
        }
    }
}

// ── Custom text presentations ───────────────────────────────────────────────
//
// A Custom node whose id is not part of the binary form
// (`schema_ir::wire_custom_id` lists those) names a text presentation of its
// inner type: `Custom { type: Int{64}, id: "AccountName" }` is a u64 on the
// wire that reads and writes as the account's name. A handler registered
// for the id converts between the PSSZ bytes of the inner type (automatic
// profile: the typed `Pssz` bytes) and the presentation text, which is
// carried as ONE JSON string in both text formats — the rule of a typed
// `TextForm`.
//
// An id with no handler falls back to the inner type's own text (the u64 as
// a number, a record as an object): the binary side is always exact, only
// the presentation is plainer, and the plain text reads back to the same
// bytes. A strict registry makes a missing handler an error instead. With a
// handler registered the presentation is exactly its text: the plain form
// is not also accepted.
//
// A handler cannot express a presentation that is not a single JSON string
// (an object, a bare number), or one that depends on the text format or on
// the surrounding value.

type ToText = dyn Fn(&[u8]) -> Result<String, PsszError> + Send + Sync;
type FromText = dyn Fn(&str) -> Result<Vec<u8>, PsszError> + Send + Sync;
/// The invariant of a custom type: whether structurally valid PSSZ bytes of
/// its wire type (automatic profile) are a value of the type. The error is
/// passed through unchanged by every entry point.
pub type Check = dyn Fn(&[u8]) -> Result<(), PsszError> + Send + Sync;

/// What is registered for a Custom id: each part is optional.
#[derive(Default)]
pub(crate) struct CustomHandler {
    /// PSSZ bytes of the wire type -> the text (unquoted).
    pub(crate) to_text: Option<Box<ToText>>,
    /// The text -> PSSZ bytes of the wire type; an error for text that is
    /// not this presentation.
    pub(crate) from_text: Option<Box<FromText>>,
    /// The invariant.
    pub(crate) check: Option<Box<Check>>,
}

/// The handlers of the custom types of a schema, by Custom id: a text form
/// (`to_text` / `from_text`), an invariant (`check`), or both.
///
/// ```text
/// let mut handlers = CustomHandlers::new();
/// handlers.add_type::<Account>()?;                 // the type's own TextForm
/// handlers.add("Version", version_to_text, version_from_text);   // by hand
/// handlers.add_check("Key", |pssz| non_empty_der(pssz));          // an invariant
/// handlers.add_invariant_type::<Range>()?;         // the type's own invariant
/// pssz_to_json_with::<IJson>(&schema, &root, bytes, &handlers)?;
/// validate_pssz_with(&schema, &root, bytes, &handlers)?;
/// ```
///
/// With a registry, `check` runs on every value of a custom node that has
/// one, on the binary and the text entry points alike: on reads after the
/// value's structure is accepted, on writes before any byte or text is
/// returned, nested values first. An id with no handler is its wire type:
/// the wire type's text, structure only. A strict registry makes a custom
/// node with no handler an error instead.
///
/// There is no bind step in this module (a schema is walked as it is
/// used): a handler is looked up by id, one hash lookup, each time a custom
/// node is read or written.
#[derive(Default)]
pub struct CustomHandlers {
    handlers: HashMap<String, CustomHandler>,
    pub(crate) strict: bool,
}

impl CustomHandlers {
    pub fn new() -> Self {
        Self::default()
    }
    /// Whether an id with no handler is an error ("no handler registered
    /// for a custom type") rather than the inner type's own text, on the
    /// binary entry points as well as the text ones.
    pub fn set_strict(&mut self, strict: bool) -> &mut Self {
        self.strict = strict;
        self
    }
    pub fn is_strict(&self) -> bool {
        self.strict
    }
    /// Registers (or replaces) the text form of `id`: `to_text` turns the
    /// PSSZ bytes of the wire type into the presentation text, `from_text`
    /// the text into those bytes. Both may fail. A `check` registered for
    /// the id is kept.
    pub fn add(
        &mut self,
        id: impl Into<String>,
        to_text: impl Fn(&[u8]) -> Result<String, PsszError> + Send + Sync + 'static,
        from_text: impl Fn(&str) -> Result<Vec<u8>, PsszError> + Send + Sync + 'static,
    ) -> &mut Self {
        let handler = self.handlers.entry(id.into()).or_default();
        handler.to_text = Some(Box::new(to_text));
        handler.from_text = Some(Box::new(from_text));
        self
    }
    /// Registers (or replaces) the invariant of `id`: whether structurally
    /// valid PSSZ bytes of the wire type are a value of the type. A text
    /// form registered for the id is kept; without one the type's text
    /// stays its wire type's.
    pub fn add_check(
        &mut self,
        id: impl Into<String>,
        check: impl Fn(&[u8]) -> Result<(), PsszError> + Send + Sync + 'static,
    ) -> &mut Self {
        self.handlers.entry(id.into()).or_default().check = Some(Box::new(check));
        self
    }
    /// Registers `T`'s own [`TextForm`] as the text form of `id`, and `T`'s
    /// typed validation (which runs its invariant and those of its members)
    /// as the check. `T`'s typed `Pssz` encoding is the wire type's bytes by
    /// construction.
    pub fn add_type_as<T>(&mut self, id: impl Into<String>) -> &mut Self
    where
        T: TextForm + PsszPack<Pssz> + PsszUnpack<Pssz> + 'static,
    {
        let id = id.into();
        self.add(
            id.clone(),
            |bytes: &[u8]| {
                let value = crate::pssz::from_pssz::<T>(bytes)?;
                let mut text = String::new();
                value.to_text(&mut text);
                Ok(text)
            },
            |text: &str| {
                let value = T::from_text(text)
                    .ok_or(PsszError::new("json: text is not a value of the custom type"))?;
                // The wire bytes of what the text says. Whether they are a
                // value of the type is the check's to report, with the
                // invariant's own message.
                let _unchecked = crate::pssz_checked::WriteUnchecked::enter();
                crate::pssz::try_encode::<Pssz, T>(&value)
            },
        );
        self.add_check(id, |bytes: &[u8]| crate::pssz::from_pssz::<T>(bytes).map(drop))
    }
    /// [`CustomHandlers::add_type_as`] under the id `T`'s `ToPsszSchema`
    /// writes into the schema (`#[psio(custom)]` / `#[psio(custom = "Id")]`).
    /// An error when `T` is not declared custom.
    pub fn add_type<T>(&mut self) -> Result<&mut Self, PsszError>
    where
        T: TextForm + ToPsszSchema + PsszPack<Pssz> + PsszUnpack<Pssz>,
    {
        let id = T::custom_id()
            .ok_or(PsszError::new("pssz dynamic: the type has no custom schema id"))?;
        Ok(self.add_type_as::<T>(id))
    }
    /// Registers `T`'s typed validation (its invariant, and those of its
    /// members) as the check of `id`: for a type with an invariant and no
    /// text form.
    pub fn add_invariant_type_as<T>(&mut self, id: impl Into<String>) -> &mut Self
    where
        T: PsszPack<Pssz> + PsszUnpack<Pssz> + 'static,
    {
        self.add_check(id, |bytes: &[u8]| crate::pssz::from_pssz::<T>(bytes).map(drop))
    }
    /// [`CustomHandlers::add_invariant_type_as`] under the id `T`'s
    /// `ToPsszSchema` writes into the schema (`#[psio(invariant = "...")]`,
    /// `#[psio(custom)]`). An error when `T` has no custom schema id.
    pub fn add_invariant_type<T>(&mut self) -> Result<&mut Self, PsszError>
    where
        T: ToPsszSchema + PsszPack<Pssz> + PsszUnpack<Pssz>,
    {
        let id = T::custom_id()
            .ok_or(PsszError::new("pssz dynamic: the type has no custom schema id"))?;
        Ok(self.add_invariant_type_as::<T>(id))
    }
    /// Whether `id` has a handler.
    pub fn contains(&self, id: &str) -> bool {
        self.handlers.contains_key(id)
    }
    /// Whether `id` has a text form / an invariant registered.
    pub fn has_text_form(&self, id: &str) -> bool {
        self.find(id).is_some_and(|h| h.to_text.is_some() || h.from_text.is_some())
    }
    pub fn has_check(&self, id: &str) -> bool {
        self.find(id).is_some_and(|h| h.check.is_some())
    }
    /// Runs the invariant registered for `id` on the PSSZ bytes of its wire
    /// type (automatic profile); `Ok` when the id has none.
    pub fn run_check(&self, id: &str, bytes: &[u8]) -> Result<(), PsszError> {
        match self.find(id).and_then(|handler| handler.check.as_ref()) {
            Some(check) => check(bytes),
            None => Ok(()),
        }
    }
    pub(crate) fn find(&self, id: &str) -> Option<&CustomHandler> {
        self.handlers.get(id)
    }
    /// Whether a binary walk has anything to do with this registry.
    pub(crate) fn checks_values(&self) -> bool {
        self.strict || self.handlers.values().any(|handler| handler.check.is_some())
    }
    /// The check of `id`, if any; an error under a strict registry that has
    /// no handler for it.
    pub(crate) fn check_of(&self, id: &str) -> Result<Option<&Check>, PsszError> {
        match self.find(id) {
            Some(handler) => Ok(handler.check.as_deref()),
            None if self.strict => Err(PsszError::new(NO_HANDLER)),
            None => Ok(None),
        }
    }
}

// ── The type walk over any cursor ───────────────────────────────────────────

/// Reads one value of `root` from `cursor` (which must be positioned at it)
/// and requires the document to end. `ijson` selects the I-JSON rules of the
/// type layer (64-bit integers, hex strings, named variants); the cursor
/// applies its own to the text.
pub fn read_json<C: JsonCursor>(
    schema: &Schema,
    root: &AnyType,
    ijson: bool,
    cursor: &mut C,
) -> Result<Value, JsonTextError> {
    read_with(schema, root, ijson, cursor, None)
}

/// [`read_json`] with the handlers of the schema's custom text forms.
pub fn read_json_with<C: JsonCursor>(
    schema: &Schema,
    root: &AnyType,
    ijson: bool,
    cursor: &mut C,
    handlers: &CustomHandlers,
) -> Result<Value, JsonTextError> {
    read_with(schema, root, ijson, cursor, Some(handlers))
}

fn read_with<C: JsonCursor>(
    schema: &Schema,
    root: &AnyType,
    ijson: bool,
    cursor: &mut C,
    handlers: Option<&CustomHandlers>,
) -> Result<Value, JsonTextError> {
    let value =
        Reader { schema, c: &mut *cursor, ijson, handlers }.read(root, IntForm::Natural, 0)?;
    cursor.finish()?;
    Ok(value)
}

// ── Text <-> Value ──────────────────────────────────────────────────────────

fn to_value<F: JsonFormat>(
    schema: &Schema,
    root: &AnyType,
    text: &[u8],
    handlers: Option<&CustomHandlers>,
) -> Result<Value, SchemaJsonError> {
    // Schema errors (unsupported shapes, bad attributes) come first.
    metadata(schema, root, true)?;
    let mut cursor = TextCursor::new(text, F::IJSON)?;
    let value = read_with(schema, root, F::IJSON, &mut cursor, handlers)?;
    run_checks(schema, root, &value, handlers)?;
    Ok(value)
}

/// The handlers' checks on a value: every custom node it holds, innermost
/// first, on its bytes. A value that merely has no encoding is not this
/// function's to report (the encoder does).
fn run_checks(
    schema: &Schema,
    root: &AnyType,
    value: &Value,
    handlers: Option<&CustomHandlers>,
) -> Result<(), PsszError> {
    match handlers.filter(|handlers| handlers.checks_values()) {
        Some(handlers) => match super::encode_checked(schema, root, value, handlers) {
            Err((error, true)) => Err(error),
            _ => Ok(()),
        },
        None => Ok(()),
    }
}

fn from_value<F: JsonFormat>(
    schema: &Schema,
    root: &AnyType,
    value: &Value,
    handlers: Option<&CustomHandlers>,
) -> Result<String, SchemaJsonError> {
    write_value::<F>(schema, root, value, handlers, true)
}

/// `checks`: whether the handlers' checks run on the value before any text
/// is returned (false when it was just decoded with the same registry).
fn write_value<F: JsonFormat>(
    schema: &Schema,
    root: &AnyType,
    value: &Value,
    handlers: Option<&CustomHandlers>,
    checks: bool,
) -> Result<String, SchemaJsonError> {
    metadata(schema, root, true)?;
    if checks {
        run_checks(schema, root, value, handlers)?;
    }
    let mut writer = ValueWriter { schema, w: Writer::new(F::IJSON), handlers };
    writer.write(root, value, 0)?;
    // The writer only emits validated UTF-8.
    String::from_utf8(writer.w.into_bytes()).map_err(|_| {
        JsonTextError { what: "json: string is not valid UTF-8".into(), offset: 0, format: F::NAME }.into()
    })
}

/// `text` (exactly one `root` value) as a [`Value`]. A custom text form
/// reads as its inner type's own text; see [`json_to_value_with`].
pub fn json_to_value<F: JsonFormat>(
    schema: &Schema,
    root: &AnyType,
    text: &[u8],
) -> Result<Value, SchemaJsonError> {
    to_value::<F>(schema, root, text, None)
}

/// [`json_to_value`] with the handlers of the schema's custom text forms.
pub fn json_to_value_with<F: JsonFormat>(
    schema: &Schema,
    root: &AnyType,
    text: &[u8],
    handlers: &CustomHandlers,
) -> Result<Value, SchemaJsonError> {
    to_value::<F>(schema, root, text, Some(handlers))
}

/// The text of `value`, a value of `root`. A custom text form is written as
/// its inner type's own text; see [`value_to_json_with`].
pub fn value_to_json<F: JsonFormat>(
    schema: &Schema,
    root: &AnyType,
    value: &Value,
) -> Result<String, SchemaJsonError> {
    from_value::<F>(schema, root, value, None)
}

/// [`value_to_json`] with the handlers of the schema's custom text forms.
pub fn value_to_json_with<F: JsonFormat>(
    schema: &Schema,
    root: &AnyType,
    value: &Value,
    handlers: &CustomHandlers,
) -> Result<String, SchemaJsonError> {
    from_value::<F>(schema, root, value, Some(handlers))
}

// ── What a schema-only consumer calls ───────────────────────────────────────

/// JSON / I-JSON text of a `root` value -> its canonical PSSZ bytes (the
/// automatic profile: the bytes of the typed `Pssz` codec).
pub fn json_to_pssz<F: JsonFormat>(
    schema: &Schema,
    root: &AnyType,
    text: &[u8],
) -> Result<Vec<u8>, SchemaJsonError> {
    let value = to_value::<F>(schema, root, text, None)?;
    Ok(super::encode::<Pssz>(schema, root, &value)?)
}

/// [`json_to_pssz`] with the handlers of the schema's custom text forms.
pub fn json_to_pssz_with<F: JsonFormat>(
    schema: &Schema,
    root: &AnyType,
    text: &[u8],
    handlers: &CustomHandlers,
) -> Result<Vec<u8>, SchemaJsonError> {
    let value = to_value::<F>(schema, root, text, Some(handlers))?;
    Ok(super::encode::<Pssz>(schema, root, &value)?)
}

/// PSSZ bytes of a `root` value -> its JSON / I-JSON text. The bytes are
/// fully validated. Unknown appended optional fields are accepted, dropped
/// from the text and reported in the second result (a caller that must not
/// lose them rejects when it is set). A raw string holding bytes that are
/// not UTF-8 is valid PSSZ but has no text: an error.
pub fn pssz_to_json<F: JsonFormat>(
    schema: &Schema,
    root: &AnyType,
    bytes: &[u8],
) -> Result<(String, bool), SchemaJsonError> {
    let decoded = super::decode_compatible::<Pssz>(schema, root, bytes)?;
    let text = from_value::<F>(schema, root, &decoded.value, None)?;
    Ok((text, decoded.has_unknown))
}

/// [`pssz_to_json`] with the handlers of the schema's custom text forms.
pub fn pssz_to_json_with<F: JsonFormat>(
    schema: &Schema,
    root: &AnyType,
    bytes: &[u8],
    handlers: &CustomHandlers,
) -> Result<(String, bool), SchemaJsonError> {
    // The decode runs the handlers' checks; the writer need not repeat them.
    let decoded =
        super::decode_compatible_with::<Pssz>(schema, root, bytes, Options::handlers(handlers))?;
    let text = write_value::<F>(schema, root, &decoded.value, Some(handlers), false)?;
    Ok((text, decoded.has_unknown))
}

/// Validates PSSZ bytes against `root` (the typed `validate` rule: unknown
/// appended optional fields are accepted and reported).
pub fn validate_pssz(
    schema: &Schema,
    root: &AnyType,
    bytes: &[u8],
) -> Result<bool, SchemaJsonError> {
    Ok(super::validate(schema, root, bytes)?)
}

/// [`validate_pssz`] with a handler registry: every value of a custom type
/// whose handler has a `check` is checked too, and a strict registry makes a
/// custom type with no handler an error.
pub fn validate_pssz_with(
    schema: &Schema,
    root: &AnyType,
    bytes: &[u8],
    handlers: &CustomHandlers,
) -> Result<bool, SchemaJsonError> {
    Ok(super::validate_with(schema, root, bytes, Options::handlers(handlers))?)
}
