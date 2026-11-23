//! [`JsonValue`]: any JSON value, without a schema (C++ `psio::json_value`).
//!
//! A document whose shape is not known when the program is compiled: null,
//! a bool, a number, a string, an array, or an object. It is a value like
//! any other to both text formats, a member of a derived record, an element
//! of a list, the payload of an `Option`.
//!
//! ```
//! use psio::{JsonValue, JsonText, PsszPack, PsszUnpack, ToPsszSchema};
//!
//! let mut v: JsonValue = psio::from_ijson(r#"{"a":1.50,"b":[true,null]}"#).unwrap();
//! assert_eq!(v.find("a").and_then(JsonValue::as_number).map(|n| n.as_str()), Some("1.50"));
//! v.set("c", "x");
//! v.erase("b");
//! assert_eq!(psio::to_ijson(&v), r#"{"a":1.50,"c":"x"}"#);
//!
//! #[derive(PsszPack, PsszUnpack, ToPsszSchema, JsonText)]
//! struct PrettyAction { service: String, method: String, data: JsonValue }
//! ```
//!
//! * A number is the token **as it was written**: `1.50`, `1e3`, `-0` and
//!   `18446744073709551616` are written back unchanged, in both formats (a
//!   `JsonValue` does not know that a number is a 64-bit integer, so it
//!   cannot give it IJson's string form). [`JsonNumber`] converts on
//!   request.
//! * An object keeps its members in the order written. Reading keeps the
//!   last value of a repeated key, at the position of the first; writing a
//!   hand-built object that holds a key twice writes its last value once.
//! * Under IJson a string holding a Unicode noncharacter is neither read
//!   nor written; nesting is at most [`MAX_DEPTH`](super::MAX_DEPTH) levels.
//! * As a record member a missing `JsonValue` is an error (`null` is a
//!   value and has to be written); `Option<JsonValue>` is the member that
//!   may be left out, and it reads `null` as `None`.
//!
//! **In PSSZ** a `JsonValue` is a UTF-8 string holding its compact `Json`
//! text: byte for byte a `String` member with that content. A reader accepts
//! any well-formed JSON text there. **In the Schema IR** it is
//! `Custom{ Custom{List<u8>, "utf8"}, "json" }`; the schema-driven codecs
//! present it as the value, inline.
//!
//! `serde`: `JsonValue` is `Serialize` / `Deserialize` through
//! [`serde_bridge`](crate::serde_bridge), and converts from and to
//! `serde_json::Value` ([`JsonValue::to_serde_json`]). psio enables no
//! `serde_json` feature for it: a `serde_json::Value` holds a number as
//! `serde_json` parsed it, so the digits of `1.50` or of an integer beyond
//! 64 bits survive only in a `JsonValue`.
use super::{kinds, JsonKind, JsonText, JsonTextError, Reader, Result, Writer, MAX_DEPTH};
use crate::pssz::{PsszError, PsszFormat, PsszPack, PsszResult, PsszUnpack, PsszValidate, Shape};
use crate::pssz_schema_ir as ir;

/// A JSON number as the text of its token (RFC 8259 `number`).
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct JsonNumber(String);

impl JsonNumber {
    /// `text` as a number, when it is exactly one RFC 8259 number token.
    pub fn new(text: impl Into<String>) -> Option<Self> {
        let text = text.into();
        let mut r = Reader::new(text.as_bytes(), false);
        (r.skip_number().is_ok() && r.pos == text.len() && !text.is_empty()).then_some(Self(text))
    }
    /// The token as it was written.
    pub fn as_str(&self) -> &str {
        &self.0
    }
    /// The value, when the token is an integer in range written without
    /// fraction or exponent.
    pub fn as_i64(&self) -> Option<i64> {
        self.0.parse().ok()
    }
    /// See [`as_i64`](Self::as_i64).
    pub fn as_u64(&self) -> Option<u64> {
        self.0.parse().ok()
    }
    /// The nearest `f64`; `None` when the value is beyond its range.
    pub fn as_f64(&self) -> Option<f64> {
        self.0.parse::<f64>().ok().filter(|v| v.is_finite())
    }
}
impl std::fmt::Display for JsonNumber {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}
macro_rules! number_from_integer {
    ($($ty:ty),*) => {$(
        impl From<$ty> for JsonNumber {
            fn from(value: $ty) -> Self {
                Self(value.to_string())
            }
        }
        impl From<$ty> for JsonValue {
            fn from(value: $ty) -> Self {
                JsonValue::Number(value.into())
            }
        }
    )*};
}
number_from_integer!(i8, i16, i32, i64, i128, u8, u16, u32, u64, u128, isize, usize);
/// A finite `f64` as its shortest round-trip text; a non-finite one has no
/// JSON text.
impl TryFrom<f64> for JsonNumber {
    type Error = JsonTextError;
    fn try_from(value: f64) -> Result<Self> {
        let text = crate::try_to_json(&value)?;
        Self::new(text).ok_or(JsonTextError {
            what: "json: a JsonValue number is not a JSON number".into(),
            offset: 0,
            format: "json",
        })
    }
}

/// Any JSON value (C++ `psio::json_value`); see the [module](self).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub enum JsonValue {
    #[default]
    Null,
    Bool(bool),
    Number(JsonNumber),
    String(String),
    Array(Vec<JsonValue>),
    /// The members in the order written.
    Object(Vec<(String, JsonValue)>),
}

impl JsonValue {
    pub fn kind(&self) -> JsonKind {
        match self {
            JsonValue::Null => JsonKind::Null,
            JsonValue::Bool(_) => JsonKind::Bool,
            JsonValue::Number(_) => JsonKind::Number,
            JsonValue::String(_) => JsonKind::String,
            JsonValue::Array(_) => JsonKind::Array,
            JsonValue::Object(_) => JsonKind::Object,
        }
    }
    pub fn is_null(&self) -> bool {
        matches!(self, JsonValue::Null)
    }
    pub fn as_bool(&self) -> Option<bool> {
        match self {
            JsonValue::Bool(value) => Some(*value),
            _ => None,
        }
    }
    pub fn as_number(&self) -> Option<&JsonNumber> {
        match self {
            JsonValue::Number(value) => Some(value),
            _ => None,
        }
    }
    pub fn as_str(&self) -> Option<&str> {
        match self {
            JsonValue::String(value) => Some(value),
            _ => None,
        }
    }
    pub fn as_array(&self) -> Option<&[JsonValue]> {
        match self {
            JsonValue::Array(items) => Some(items),
            _ => None,
        }
    }
    pub fn as_object(&self) -> Option<&[(String, JsonValue)]> {
        match self {
            JsonValue::Object(members) => Some(members),
            _ => None,
        }
    }
    /// The value of member `key` of an object (the last one, when a
    /// hand-built object holds the key twice).
    pub fn find(&self, key: &str) -> Option<&JsonValue> {
        self.as_object()?.iter().rev().find(|(name, _)| name == key).map(|(_, value)| value)
    }
    /// Sets member `key`: replaced in place, or appended. Anything but an
    /// object becomes an object first.
    pub fn set(&mut self, key: impl Into<String>, value: impl Into<JsonValue>) -> &mut Self {
        if !matches!(self, JsonValue::Object(_)) {
            *self = JsonValue::Object(Vec::new());
        }
        let (key, value) = (key.into(), value.into());
        if let JsonValue::Object(members) = self {
            match members.iter_mut().find(|(name, _)| *name == key) {
                Some(member) => member.1 = value,
                None => members.push((key, value)),
            }
        }
        self
    }
    /// Removes member `key`; whether there was one.
    pub fn erase(&mut self, key: &str) -> bool {
        match self {
            JsonValue::Object(members) => {
                let before = members.len();
                members.retain(|(name, _)| name != key);
                members.len() != before
            }
            _ => false,
        }
    }
    /// The value as a `serde_json::Value`: numbers as `serde_json` parses
    /// them (an error for one beyond its range).
    pub fn to_serde_json(&self) -> std::result::Result<serde_json::Value, serde_json::Error> {
        let text = crate::try_to_json(self).map_err(<serde_json::Error as serde::ser::Error>::custom)?;
        serde_json::from_str(&text)
    }

    fn write(&self, w: &mut Writer, depth: usize) -> Result<()> {
        match self {
            JsonValue::Null => w.raw("null"),
            JsonValue::Bool(value) => w.raw(if *value { "true" } else { "false" }),
            JsonValue::Number(number) => w.raw(&number.0),
            JsonValue::String(text) => w.string(text.as_bytes())?,
            JsonValue::Array(items) => {
                if depth >= MAX_DEPTH {
                    return Err(w.err("json: max depth exceeded"));
                }
                w.raw("[");
                for (i, item) in items.iter().enumerate() {
                    if i > 0 {
                        w.raw(",");
                    }
                    item.write(w, depth + 1)?;
                }
                w.raw("]");
            }
            JsonValue::Object(members) => {
                if depth >= MAX_DEPTH {
                    return Err(w.err("json: max depth exceeded"));
                }
                w.raw("{");
                let mut first = true;
                for (i, (key, value)) in members.iter().enumerate() {
                    // A key held twice is written once, where it first
                    // stands, with its last value.
                    if members[..i].iter().any(|(earlier, _)| earlier == key) {
                        continue;
                    }
                    let value =
                        members[i + 1..].iter().rev().find(|(later, _)| later == key).map_or(value, |(_, v)| v);
                    w.key(key, first)?;
                    first = false;
                    value.write(w, depth + 1)?;
                }
                w.raw("}");
            }
        }
        Ok(())
    }
}

impl From<bool> for JsonValue {
    fn from(value: bool) -> Self {
        JsonValue::Bool(value)
    }
}
impl From<JsonNumber> for JsonValue {
    fn from(value: JsonNumber) -> Self {
        JsonValue::Number(value)
    }
}
impl From<&str> for JsonValue {
    fn from(value: &str) -> Self {
        JsonValue::String(value.into())
    }
}
impl From<String> for JsonValue {
    fn from(value: String) -> Self {
        JsonValue::String(value)
    }
}
impl From<Vec<JsonValue>> for JsonValue {
    fn from(value: Vec<JsonValue>) -> Self {
        JsonValue::Array(value)
    }
}
impl From<Vec<(String, JsonValue)>> for JsonValue {
    fn from(value: Vec<(String, JsonValue)>) -> Self {
        JsonValue::Object(value)
    }
}
impl<T: Into<JsonValue>> From<Option<T>> for JsonValue {
    fn from(value: Option<T>) -> Self {
        value.map_or(JsonValue::Null, Into::into)
    }
}
/// Through the text `serde_json` writes: a number is the token it prints.
impl From<&serde_json::Value> for JsonValue {
    fn from(value: &serde_json::Value) -> Self {
        // serde_json writes one well-formed JSON value.
        crate::from_json(value.to_string()).unwrap_or_default()
    }
}
impl From<serde_json::Value> for JsonValue {
    fn from(value: serde_json::Value) -> Self {
        (&value).into()
    }
}

impl JsonText for JsonValue {
    const JSON_KINDS: u8 = kinds::ANY;
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        self.write(w, 0)
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        Ok(match r.kind()? {
            JsonKind::Null => {
                if !r.keyword("null") {
                    return Err(r.err("json: malformed keyword"));
                }
                JsonValue::Null
            }
            JsonKind::Bool => JsonValue::Bool(bool::read_json(r)?),
            JsonKind::Number => {
                let begin = r.pos;
                r.skip_number()?;
                // The number grammar admits ASCII only.
                match std::str::from_utf8(&r.src[begin..r.pos]) {
                    Ok(token) => JsonValue::Number(JsonNumber(token.into())),
                    Err(_) => return Err(r.err_at(begin, "json: expected a number")),
                }
            }
            JsonKind::String => JsonValue::String(r.string()?.into_owned()),
            JsonKind::Array => {
                let mut items = Vec::new();
                r.begin(b'[', "json: expected '['")?;
                if !r.take(b']') {
                    loop {
                        items.push(Self::read_json(r)?);
                        if r.take(b']') {
                            break;
                        }
                        if !r.take(b',') {
                            return Err(r.err("json: expected ',' or ']' in array"));
                        }
                    }
                }
                r.end();
                JsonValue::Array(items)
            }
            JsonKind::Object => {
                let mut members: Vec<(String, JsonValue)> = Vec::new();
                r.begin(b'{', "json: expected '{'")?;
                if !r.take(b'}') {
                    loop {
                        let key = r.key()?.into_owned();
                        let value = Self::read_json(r)?;
                        // The last value of a repeated key, at the position
                        // of the first.
                        match members.iter_mut().find(|(name, _)| *name == key) {
                            Some(member) => member.1 = value,
                            None => members.push((key, value)),
                        }
                        if r.object_next()? {
                            break;
                        }
                    }
                }
                r.end_object();
                JsonValue::Object(members)
            }
        })
    }
}

// ── PSSZ: a UTF-8 string holding the compact Json text ──────────────────────

const NOT_JSON: PsszError = PsszError::new("pssz: the text of a json value is not JSON");

impl JsonValue {
    /// The compact `Json` text: what the PSSZ string holds.
    fn compact(&self) -> PsszResult<String> {
        crate::try_to_json(self).map_err(|_| PsszError::new("pssz: a json value has no JSON text"))
    }
    fn of_text(text: &str) -> PsszResult<Self> {
        crate::from_json(text).map_err(|_| NOT_JSON)
    }
}
impl<F: PsszFormat> PsszPack<F> for JsonValue {
    const SHAPE: Shape = Shape::leaf("json", 0, false);
    const MIN_ENCODED_SIZE: usize = 0;
    const MAX_ENCODED_SIZE: Option<usize> = None;
    const IS_FIXED_SIZE: bool = false;
    const FIXED_SIZE: usize = 0;
    fn pssz_size(&self) -> usize {
        self.compact().map_or(0, |text| text.len())
    }
    /// Panics for a value nested deeper than a reader accepts; fallible
    /// callers use `try_to_pssz`.
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        let text = self.compact().expect("psio: a json value has no JSON text");
        out.extend_from_slice(text.as_bytes());
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        <String as PsszPack<F>>::pssz_pack_checked(&self.compact()?, out)
    }
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        <String as PsszPack<F>>::pssz_pack_at_depth(&self.compact()?, out, depth)
    }
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
        <String as PsszPack<F>>::pssz_size_at_depth(&self.compact()?, depth)
    }
}
impl<F: PsszFormat> PsszUnpack<F> for JsonValue {
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        Self::of_text(&<String as PsszUnpack<F>>::pssz_unpack(bytes)?)
    }
    fn pssz_unpack_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Self> {
        Self::of_text(&<String as PsszUnpack<F>>::pssz_unpack_at_depth(bytes, depth)?)
    }
}
impl<F: PsszFormat> PsszValidate<F> for JsonValue {
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        <String as PsszValidate<F>>::pssz_validate(bytes)?;
        super::compact_json(bytes, false).map(|_| ()).map_err(|_| NOT_JSON)
    }
    fn pssz_validate_at_depth(bytes: &[u8], depth: usize) -> PsszResult<()> {
        <String as PsszValidate<F>>::pssz_validate_at_depth(bytes, depth)?;
        super::compact_json(bytes, false).map(|_| ()).map_err(|_| NOT_JSON)
    }
}
impl ir::ToPsszSchema for JsonValue {
    fn build_schema(builder: &mut ir::SchemaBuilder) -> PsszResult<ir::AnyType> {
        Ok(ir::AnyType::new(ir::AnyValue::Custom(ir::Custom {
            r#type: Box::new(<String as ir::ToPsszSchema>::schema(builder)?),
            id: "json".into(),
        })))
    }
}

// ── serde: the value's own text ─────────────────────────────────────────────

impl serde::Serialize for JsonValue {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> std::result::Result<S::Ok, S::Error> {
        crate::serde_bridge::serialize(self, serializer)
    }
}
impl<'de> serde::Deserialize<'de> for JsonValue {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> std::result::Result<Self, D::Error> {
        crate::serde_bridge::deserialize(deserializer)
    }
}
