//! serde for PSIO types: `Serialize` / `Deserialize` whose behaviour is
//! PSIO's JSON rules.
//!
//! A type annotated once with the PSIO derives gets serde impls from
//! `#[derive(psio::Serde)]`. They are defined by the type's
//! [`JsonText`](crate::json::JsonText), under the I-JSON rules
//! ([`IJson`](crate::IJson)), so there is one description of the type and
//! one text:
//!
//! * a 64-bit integer beyond 2^53 - 1 is a decimal string, a 128- or 256-bit
//!   integer always is, a byte sequence is one uppercase hex string;
//! * a variant is `{"Name": value}` and an enumerator is its name;
//! * `String` is UTF-8 text and `RawString` raw bytes, as PSIO writes each;
//! * a missing optional member is `None`, unknown members are skipped, any
//!   other missing member is an error;
//! * invariants (`#[psio(invariant = "path")]`) are enforced in both
//!   directions, with their own message.
//!
//! ```
//! use psio::{JsonText, PsszPack, PsszUnpack, Serde};
//!
//! #[derive(Debug, PartialEq, PsszPack, PsszUnpack, JsonText, Serde)]
//! struct Transfer {
//!     to: String,
//!     amount: u64,
//!     memo: Option<String>,
//! }
//!
//! let transfer = Transfer { to: "bob".into(), amount: u64::MAX, memo: None };
//! let text = serde_json::to_string(&transfer)?;
//! assert_eq!(text, r#"{"to":"bob","amount":"18446744073709551615","memo":null}"#);
//! assert_eq!(text, psio::to_ijson(&transfer));
//! // A missing optional member is None; an unknown member is skipped.
//! let read: Transfer = serde_json::from_str(r#"{"to":"bob","amount":"18446744073709551615","x":1}"#)?;
//! assert_eq!(read, transfer);
//! // A bare number beyond 2^53 - 1 is rejected, not rounded.
//! assert!(serde_json::from_str::<Transfer>(r#"{"to":"bob","amount":18446744073709551615}"#).is_err());
//! # Ok::<(), serde_json::Error>(())
//! ```
//!
//! PSIO's own types that have a JSON text implement both traits the same
//! way ([`RawString`](crate::pssz::RawString), the bounded strings, lists,
//! maps and sets, [`Nested`](crate::pssz::Nested),
//! [`Uint256`](crate::pssz::Uint256), the [`time`](crate::time) types,
//! [`ByKey`](crate::key::ByKey)), so a struct that uses serde's own derive
//! and holds them gets PSIO's text for those members. The functions here
//! have the signatures `#[serde(with = "psio::serde_bridge")]` expects, for
//! a member of any other [`JsonText`](crate::json::JsonText) type.
//!
//! # How it works, and what it costs
//!
//! Serializing writes the IJson text of the value, parses it, and replays
//! it into the serializer as serde data-model values: a record or named
//! variant is a map with string keys, an array a sequence (both with their
//! length), a string a `str`, `null` a unit, a number a `u64` / `i64` when
//! it is an integer in range and an `f64` otherwise.
//!
//! Deserializing turns what the deserializer holds into JSON text and reads
//! that with [`from_ijson`](crate::from_ijson), so what is accepted is what
//! PSIO accepts. `serde_json` hands over the text of the value exactly as
//! it stands in the input. Any other format is asked for whatever it holds
//! (`deserialize_any`), which is written back as JSON text.
//!
//! Each direction costs one text round trip and its allocations on top of
//! the format's own work; use [`to_ijson`](crate::to_ijson) /
//! [`from_ijson`](crate::from_ijson) directly when the text is all that is
//! wanted.
//!
//! # Limits
//!
//! * **Reading with `serde_json` accepts and rejects exactly what
//!   [`from_ijson`](crate::from_ijson) does**, with the same value and, for
//!   a rejection by the type, the same message.
//! * **Writing with `serde_json` gives PSIO's IJson text byte for byte,
//!   except two spellings** serde's data model does not carry. A float that
//!   is not an integer of at most 64 bits is an `f64`, which `serde_json`
//!   spells its own way: PSIO `1e+15`, `1e-07`, `-0`,
//!   `123456789012345683968`; `serde_json` `1000000000000000.0`, `1e-7`,
//!   `-0.0`, `1.2345678901234568e+20`. A string is a `str`, in which
//!   `serde_json` escapes U+0008 and U+000C as `\b` and `\f` where PSIO
//!   writes `\u0008` and `\u000c`. The value is the same and each side
//!   reads the other's text. Use [`to_ijson`](crate::to_ijson) where the
//!   bytes matter (a hash, a signature).
//! * **Reading needs a self-describing format.** A format whose reader
//!   must be told the type (bincode, postcard) cannot implement
//!   `deserialize_any`, and returns its own error for it.
//! * **Writing to such a format** produces the data-model values above: a
//!   record is a length-prefixed map of string keys, not that format's
//!   usual struct encoding, and nothing can read it back through this
//!   bridge. PSIO's binary format is pSSZ.
//! * A format's byte-string type is not read: in PSIO text bytes are an
//!   uppercase hex string. A map key must be a string.
//! * A self-describing format other than `serde_json` hands over the
//!   `u64` / `i64` / `f64` it parsed, not the digits of its input: a float
//!   is read from its shortest round-trip text.
//! * Only PSIO types are affected. A `String`, `u64` or `Vec<u8>` member of
//!   a struct that uses serde's own derive keeps serde's impl; give the
//!   struct `#[derive(psio::JsonText, psio::Serde)]`, or the member
//!   `#[serde(with = "psio::serde_bridge")]`, for PSIO's text.
//!
//! psio builds `serde_json` with `arbitrary_precision` and `raw_value`,
//! which is how a number and a value reach this module as text.

use crate::json::{JsonCursor, JsonKind, JsonText, JsonTextError, TextCursor};
use serde::de::{self, DeserializeSeed, Deserializer, MapAccess, SeqAccess, Visitor};
use serde::ser::{self, SerializeMap, SerializeSeq, Serializer};
use std::fmt;

/// The name `serde_json` built with `arbitrary_precision` gives the one
/// member of the map it presents a number as; its value is the number's
/// text.
const NUMBER_TOKEN: &str = "$serde_json::private::Number";

/// The newtype name `serde_json` built with `raw_value` answers with a map
/// of one member of the same name, holding the exact text of the value.
const RAW_TOKEN: &str = "$serde_json::private::RawValue";

/// Serializes `value` as its IJson text, replayed into `serializer`.
pub fn serialize<T: JsonText, S: Serializer>(value: &T, serializer: S) -> Result<S::Ok, S::Error> {
    let text = crate::try_to_ijson(value).map_err(ser::Error::custom)?;
    let mut cursor = TextCursor::new(text.as_bytes(), true).map_err(ser::Error::custom)?;
    let tree = Node::read(&mut cursor).map_err(ser::Error::custom)?;
    ser::Serialize::serialize(&tree, serializer)
}

/// Deserializes a `T` from whatever `deserializer` holds, read as IJson
/// text: accepts and rejects what [`from_ijson`](crate::from_ijson) does.
pub fn deserialize<'de, T: JsonText, D: Deserializer<'de>>(deserializer: D) -> Result<T, D::Error> {
    let mut text = String::new();
    // serde_json hands over the text as it stands in the input; any other
    // format passes itself to `visit_newtype_struct` and is asked for what
    // it holds.
    deserializer.deserialize_newtype_struct(RAW_TOKEN, Text { out: &mut text, top: true })?;
    crate::from_ijson(&text).map_err(de::Error::custom)
}

// ── Writing: JSON text as data-model values ─────────────────────────────────

/// A JSON text as a tree, so a sequence or map is serialized with its
/// length. A number keeps its token.
enum Node {
    Null,
    Bool(bool),
    Number(String),
    String(String),
    Array(Vec<Node>),
    Object(Vec<(String, Node)>),
}

impl Node {
    fn read(cursor: &mut TextCursor<'_>) -> Result<Node, JsonTextError> {
        Ok(match cursor.kind()? {
            JsonKind::Null => {
                cursor.null()?;
                Node::Null
            }
            JsonKind::Bool => Node::Bool(cursor.boolean()?),
            JsonKind::Number => Node::Number(cursor.number()?.to_string()),
            JsonKind::String => Node::String(cursor.string()?.0.into_owned()),
            JsonKind::Array => {
                cursor.begin_array()?;
                let mut items = Vec::new();
                while cursor.next_element()? {
                    items.push(Node::read(cursor)?);
                }
                Node::Array(items)
            }
            JsonKind::Object => {
                cursor.begin_object()?;
                let mut members = Vec::new();
                loop {
                    let Some(name) = cursor.next_field()?.map(|name| name.into_owned()) else {
                        break;
                    };
                    members.push((name, Node::read(cursor)?));
                }
                Node::Object(members)
            }
        })
    }
}

impl ser::Serialize for Node {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        match self {
            Node::Null => serializer.serialize_unit(),
            Node::Bool(value) => serializer.serialize_bool(*value),
            Node::Number(token) => {
                // "-0" is the float; the integer zero has no sign.
                if let Ok(value) = token.parse::<u64>() {
                    serializer.serialize_u64(value)
                } else if let (Ok(value), false) = (token.parse::<i64>(), token == "-0") {
                    serializer.serialize_i64(value)
                } else {
                    let value: f64 = token.parse().map_err(ser::Error::custom)?;
                    serializer.serialize_f64(value)
                }
            }
            Node::String(text) => serializer.serialize_str(text),
            Node::Array(items) => {
                let mut seq = serializer.serialize_seq(Some(items.len()))?;
                for item in items {
                    seq.serialize_element(item)?;
                }
                seq.end()
            }
            Node::Object(members) => {
                let mut map = serializer.serialize_map(Some(members.len()))?;
                for (name, value) in members {
                    map.serialize_entry(name, value)?;
                }
                map.end()
            }
        }
    }
}

// ── Reading: data-model values as JSON text ─────────────────────────────────

/// Appends the JSON text of the next value to the string. `top`: the value
/// is the whole input, which serde_json presents as its raw text.
struct Text<'a> {
    out: &'a mut String,
    top: bool,
}
impl<'a> Text<'a> {
    fn inner(out: &'a mut String) -> Self {
        Text { out, top: false }
    }
}

fn push_string(out: &mut String, text: &str) {
    out.push('"');
    for c in text.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
}

/// Whether `text` is made of the characters of a JSON number. What it says
/// is the reader's to judge.
fn number_like(text: &str) -> bool {
    !text.is_empty() && text.bytes().all(|b| b.is_ascii_digit() || b"+-.eE".contains(&b))
}

impl<'de> DeserializeSeed<'de> for Text<'_> {
    type Value = ();
    fn deserialize<D: Deserializer<'de>>(self, deserializer: D) -> Result<(), D::Error> {
        deserializer.deserialize_any(self)
    }
}

impl<'de> Visitor<'de> for Text<'_> {
    type Value = ();

    fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("a value with a JSON text")
    }
    fn visit_bool<E>(self, value: bool) -> Result<(), E> {
        self.out.push_str(if value { "true" } else { "false" });
        Ok(())
    }
    fn visit_i64<E>(self, value: i64) -> Result<(), E> {
        self.out.push_str(&value.to_string());
        Ok(())
    }
    fn visit_u64<E>(self, value: u64) -> Result<(), E> {
        self.out.push_str(&value.to_string());
        Ok(())
    }
    fn visit_i128<E>(self, value: i128) -> Result<(), E> {
        self.out.push_str(&value.to_string());
        Ok(())
    }
    fn visit_u128<E>(self, value: u128) -> Result<(), E> {
        self.out.push_str(&value.to_string());
        Ok(())
    }
    fn visit_f64<E: de::Error>(self, value: f64) -> Result<(), E> {
        if !value.is_finite() {
            return Err(E::custom("json: NaN and the infinities have no text"));
        }
        self.out.push_str(ryu::Buffer::new().format_finite(value));
        Ok(())
    }
    fn visit_str<E>(self, value: &str) -> Result<(), E> {
        push_string(self.out, value);
        Ok(())
    }
    fn visit_bytes<E: de::Error>(self, _: &[u8]) -> Result<(), E> {
        Err(E::custom("json: a byte string has no JSON text; PSIO bytes are an uppercase hex string"))
    }
    fn visit_unit<E>(self) -> Result<(), E> {
        self.out.push_str("null");
        Ok(())
    }
    fn visit_none<E>(self) -> Result<(), E> {
        self.out.push_str("null");
        Ok(())
    }
    fn visit_some<D: Deserializer<'de>>(self, deserializer: D) -> Result<(), D::Error> {
        deserializer.deserialize_any(Text::inner(self.out))
    }
    fn visit_newtype_struct<D: Deserializer<'de>>(self, deserializer: D) -> Result<(), D::Error> {
        deserializer.deserialize_any(Text::inner(self.out))
    }
    fn visit_seq<A: SeqAccess<'de>>(self, mut seq: A) -> Result<(), A::Error> {
        self.out.push('[');
        let mut first = true;
        loop {
            let mark = self.out.len();
            if !first {
                self.out.push(',');
            }
            if seq.next_element_seed(Text::inner(self.out))?.is_none() {
                self.out.truncate(mark);
                break;
            }
            first = false;
        }
        self.out.push(']');
        Ok(())
    }
    fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<(), A::Error> {
        let start = self.out.len();
        self.out.push('{');
        let mut first = true;
        loop {
            let mark = self.out.len();
            if !first {
                self.out.push(',');
            }
            let name_at = self.out.len();
            if map.next_key_seed(Name(self.out))?.is_none() {
                self.out.truncate(mark);
                break;
            }
            if first && self.top && self.out[name_at..] == format!("\"{RAW_TOKEN}\"") {
                // The exact text of the value.
                let raw: String = map.next_value()?;
                self.out.truncate(start);
                self.out.push_str(&raw);
                return Ok(());
            }
            if first && self.out[name_at..] == format!("\"{NUMBER_TOKEN}\"") {
                // serde_json presents a number as a map of this one member
                // holding the number's text.
                let token: String = map.next_value()?;
                if !number_like(&token) {
                    return Err(de::Error::custom("json: malformed number"));
                }
                self.out.truncate(start);
                self.out.push_str(&token);
                return Ok(());
            }
            self.out.push(':');
            map.next_value_seed(Text::inner(self.out))?;
            first = false;
        }
        self.out.push('}');
        Ok(())
    }
}

/// Appends a member name. A JSON object has string names.
struct Name<'a>(&'a mut String);

impl<'de> DeserializeSeed<'de> for Name<'_> {
    type Value = ();
    fn deserialize<D: Deserializer<'de>>(self, deserializer: D) -> Result<(), D::Error> {
        deserializer.deserialize_any(self)
    }
}

impl<'de> Visitor<'de> for Name<'_> {
    type Value = ();

    fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("a string member name")
    }
    fn visit_str<E>(self, value: &str) -> Result<(), E> {
        push_string(self.0, value);
        Ok(())
    }
}

// ── PSIO's own types ────────────────────────────────────────────────────────

macro_rules! serde_as_text {
    ($([$($generics:tt)*] $ty:ty;)*) => {$(
        impl<$($generics)*> serde::Serialize for $ty
        where
            Self: JsonText,
        {
            fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
                serialize(self, serializer)
            }
        }
        impl<'de, $($generics)*> serde::Deserialize<'de> for $ty
        where
            Self: JsonText,
        {
            fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
                deserialize(deserializer)
            }
        }
    )*};
}

serde_as_text! {
    [] crate::pssz::RawString;
    [const N: usize] crate::pssz::BoundedString<N>;
    [const N: usize] crate::pssz::BoundedRawString<N>;
    [T, const N: usize] crate::pssz::BoundedList<T, N>;
    [K, V, const N: usize] crate::pssz::BoundedMap<K, V, N>;
    [K, const N: usize] crate::pssz::BoundedSet<K, N>;
    [T, V, const N: usize] crate::pssz::Nested<T, V, N>;
    [] crate::pssz::Uint256;
    [U, R] crate::time::TimePoint<U, R>;
    [U, R] crate::time::MonotonicPoint<U, R>;
    [U, R] crate::time::Duration<U, R>;
    [T] crate::key::ByKey<T>;
}
