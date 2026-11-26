// The JSON text formats `Json` (RFC 8259) and `IJson` (RFC 7493), the Rust
// counterpart of C++ `psio::json` / `psio::ijson` (cpp/include/psio/json.hpp).
// Both languages produce the same text for the same value and accept and
// reject the same input; conformance/json-vectors.json is the shared proof.
//
//   Json    every integer is a bare number, a byte sequence is an array of
//           numbers, an enum with payloads is `[index, value]`.
//   IJson   a 64-bit integer is a bare number while its magnitude is at most
//           2^53 - 1 and a decimal string beyond that (either form is read,
//           a bare number only within that range), a 128- or 256-bit integer
//           (`u128`, `i128`, `Uint256`) is always a decimal string, a byte
//           sequence (`Vec<u8>`, `[u8; N]`) is one UPPERCASE hex string, an
//           enum with payloads is `{"Name": value}`.
//
// Everything else is common: records are objects in declaration order,
// `Vec` / arrays / tuples are arrays (`()` is `[]`), `Option` is null or the
// value, `String` and `RawString` are JSON strings. A field-less
// `#[repr(int)]` enum is its variant's name as a string; it is read from the
// name, the number, the numeric string, or `"name:number"` (which must
// agree). A value held by the enum's `#[psio(other)]` catch-all is written
// as its number. NaN and the infinities have no text: writing one is an
// error in both formats.
//
// 128- and 256-bit integers are also read from a decimal string in `Json`,
// from a bare number (`IJson`: within 2^53 - 1) and from the older
// `"0x<hex>"` string; they are always written in decimal.
//
// Reading is strict and typed (see `Reader`): canonical in-range integers,
// exact arity, valid UTF-8, at most `MAX_DEPTH` nested arrays/objects.
// Unknown object keys are skipped; a missing member is an error unless it is
// an `Option`. A repeated key is read each time it appears and the last one
// wins, in both formats (single pass, no seen-key bookkeeping); each
// occurrence must be well-typed.

use crate::pssz::RawString;
use crate::{Decode, Encode, Format, Validate, ValidationPolicy};
use std::borrow::Cow;

/// Maximum nesting of arrays and objects accepted by the reader (C++
/// `kMaxValidationDepth`).
pub const MAX_DEPTH: usize = 64;

/// Largest magnitude a JSON number carries exactly through JavaScript.
const SAFE_INTEGER: u64 = (1 << 53) - 1;

/// Why text was rejected, or why a value has no JSON text.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct JsonTextError {
    /// Description: a constant (`Cow::Borrowed`, no allocation) or text
    /// computed from the value (`Cow::Owned`), which is what a type's
    /// invariant may return.
    pub what: Cow<'static, str>,
    /// Byte offset in the input (reading) or output (writing).
    pub offset: usize,
    /// "json" or "ijson".
    pub format: &'static str,
}
impl std::fmt::Display for JsonTextError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} (at byte {} of {})", self.what, self.offset, self.format)
    }
}
impl std::error::Error for JsonTextError {}

type Result<T> = std::result::Result<T, JsonTextError>;

/// A JSON text format tag: [`Json`] or [`IJson`].
pub trait JsonFormat: Format<Error = JsonTextError> {
    const IJSON: bool;
    const NAME: &'static str;
}

/// RFC 8259 JSON: the counterpart of C++ `psio::json`.
pub struct Json;
/// RFC 7493 I-JSON: the counterpart of C++ `psio::ijson`.
pub struct IJson;

impl Format for Json {
    type Error = JsonTextError;
}
impl Format for IJson {
    type Error = JsonTextError;
}
impl JsonFormat for Json {
    const IJSON: bool = false;
    const NAME: &'static str = "json";
}
impl JsonFormat for IJson {
    const IJSON: bool = true;
    const NAME: &'static str = "ijson";
}

/// How an integer is written in the current position.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IntForm {
    /// The format's rule.
    Natural,
    /// Forced by `#[json(json_number)]` / `#[json(ijson_number)]`.
    Number,
    /// Forced by `#[json(json_string)]` / `#[json(ijson_string)]`.
    String,
}

/// A type with a JSON / I-JSON text. Derive it with
/// `#[derive(psio::JsonText)]`, or give a type a custom string form with
/// [`TextForm`] and `psio::json_text_form!`.
pub trait JsonText: Sized {
    /// The text of the type is a JSON string *by type* (a string kind, a
    /// custom text form, or a transparent wrapper of one): a map keyed by it
    /// is a JSON object. A type that merely happens to be written as a
    /// string (an enumerator name, a time point, hex bytes) is not.
    const JSON_STRING: bool = false;

    /// The kinds of JSON value the type may be written as, in either format
    /// (a bit set of [`kinds`]; docs/json.md "Untagged variants"): what lets
    /// a reader pick the alternative of an untagged enum
    /// (`#[psio(untagged)]`) without trying each. A hand-written impl that
    /// does not say is taken to be written as any kind, so it cannot be told
    /// apart from another alternative.
    const JSON_KINDS: u8 = kinds::ANY;
    /// [`JSON_KINDS`](Self::JSON_KINDS) of a sequence of `Self`: an array,
    /// and for bytes also a string.
    #[doc(hidden)]
    const JSON_SEQUENCE_KINDS: u8 = kinds::ARRAY;

    fn write_json(&self, w: &mut Writer) -> Result<()>;
    fn read_json(r: &mut Reader<'_>) -> Result<Self>;

    /// A sequence of `Self`. `u8` overrides these: bytes are hex in IJson.
    fn write_json_slice(items: &[Self], w: &mut Writer) -> Result<()> {
        w.out.push(b'[');
        for (i, item) in items.iter().enumerate() {
            if i > 0 {
                w.out.push(b',');
            }
            item.write_json(w)?;
        }
        w.out.push(b']');
        Ok(())
    }
    fn read_json_vec(r: &mut Reader<'_>) -> Result<Vec<Self>> {
        let mut out = Vec::new();
        r.begin(b'[', "json: expected '['")?;
        if !r.take(b']') {
            loop {
                out.push(Self::read_json(r)?);
                if r.take(b']') {
                    break;
                }
                if !r.take(b',') {
                    return Err(r.err("json: expected ',' or ']' in array"));
                }
            }
        }
        r.end();
        Ok(out)
    }
    fn read_json_array<const N: usize>(r: &mut Reader<'_>) -> Result<[Self; N]> {
        let mut out = Vec::with_capacity(N);
        r.begin(b'[', "json: expected '['")?;
        for i in 0..N {
            r.element(i)?;
            out.push(Self::read_json(r)?);
        }
        r.close_array()?;
        out.try_into().map_err(|_| r.err("json: array has the wrong length"))
    }

    /// The value of a record member that is absent from the text: `None`
    /// for an `Option`, otherwise no value (the member is required).
    fn json_missing() -> Option<Self> {
        None
    }
}

/// A custom string form, the counterpart of a C++ text adapter
/// (`PSIO_ADAPTER(T, psio::text_category, ...)`). Register it with
/// `psio::json_text_form!(Type);` and the type is written as a JSON string
/// holding this text in both formats.
pub trait TextForm: Sized {
    fn to_text(&self, out: &mut String);
    /// `None` when the text is not a value.
    fn from_text(text: &str) -> Option<Self>;
}

/// Implements [`JsonText`] for a type through its [`TextForm`].
///
/// `json_text_form!(Type, invariant = path)` also enforces the type's
/// invariant (the `fn(&Type) -> Result<(), E>`, `E: Into<Cow<'static, str>>`,
/// named by its `#[psio(invariant = "path")]`): the text form says what is well-formed
/// text, the invariant whether the value read is a value of the type.
#[macro_export]
macro_rules! json_text_form {
    ($ty:ty, invariant = $check:path) => {
        impl $crate::json::JsonText for $ty {
            const JSON_STRING: bool = true;
            const JSON_KINDS: u8 = $crate::json::kinds::STRING;
            fn write_json(
                &self,
                w: &mut $crate::json::Writer,
            ) -> ::core::result::Result<(), $crate::json::JsonTextError> {
                if let ::core::result::Result::Err(why) = $check(self) {
                    return ::core::result::Result::Err(w.err(why));
                }
                let mut text = ::std::string::String::new();
                <$ty as $crate::json::TextForm>::to_text(self, &mut text);
                w.string(text.as_bytes())
            }
            fn read_json(
                r: &mut $crate::json::Reader<'_>,
            ) -> ::core::result::Result<Self, $crate::json::JsonTextError> {
                r.ws();
                let at = r.position();
                let text = r.string()?;
                let value = <$ty as $crate::json::TextForm>::from_text(&text)
                    .ok_or_else(|| r.err_at(at, "json: text form rejected the value"))?;
                match $check(&value) {
                    ::core::result::Result::Ok(()) => ::core::result::Result::Ok(value),
                    ::core::result::Result::Err(why) => {
                        ::core::result::Result::Err(r.err_at(at, why))
                    }
                }
            }
        }
    };
    ($ty:ty) => {
        impl $crate::json::JsonText for $ty {
            const JSON_STRING: bool = true;
            const JSON_KINDS: u8 = $crate::json::kinds::STRING;
            fn write_json(
                &self,
                w: &mut $crate::json::Writer,
            ) -> ::core::result::Result<(), $crate::json::JsonTextError> {
                let mut text = ::std::string::String::new();
                <$ty as $crate::json::TextForm>::to_text(self, &mut text);
                w.string(text.as_bytes())
            }
            fn read_json(
                r: &mut $crate::json::Reader<'_>,
            ) -> ::core::result::Result<Self, $crate::json::JsonTextError> {
                let at = r.position();
                let text = r.string()?;
                <$ty as $crate::json::TextForm>::from_text(&text)
                    .ok_or_else(|| r.err_at(at, "json: text form rejected the value"))
            }
        }
    };
}

// ── Entry points ────────────────────────────────────────────────────────────
//
// Two layers, as in C++ (docs/helpers.md):
//
//   - The helper names `to_json` / `try_to_json` / `to_json_into` /
//     `from_json` / `from_json_trusted` / `validate_json` / `json_size` and
//     the same seven for `ijson`. They are what most code calls.
//   - The tag-generic functions `encode` / `try_encode` / `encode_into` /
//     `decode` / `validate` / `size_of` take the format tag ([`Json`] or
//     [`IJson`]) as their first type argument, for code generic over it.
//
// Text is taken as `impl AsRef<[u8]>`: a `&str`, `String`, `&[u8]` or
// `Vec<u8>`.

/// The text of `value`, or why it has none (NaN, an infinity, a `RawString`
/// that is not UTF-8, a violated invariant).
pub fn try_encode<F: JsonFormat, T: JsonText>(value: &T) -> Result<String> {
    // The validated write validates whatever the caller is in the middle of.
    let _validating = crate::pssz_checked::Declared::enter();
    write_text::<F, T>(value)
}

// The text, in whatever mode the thread is in.
fn write_text<F: JsonFormat, T: JsonText>(value: &T) -> Result<String> {
    let mut w = Writer::new(F::IJSON);
    value.write_json(&mut w)?;
    // The writer only emits validated UTF-8.
    Ok(String::from_utf8(w.out).expect("the JSON writer emits UTF-8"))
}

/// The text of `value`. Panics when the value has no text; use
/// [`try_encode`] where that can depend on data.
#[track_caller]
pub fn encode<F: JsonFormat, T: JsonText>(value: &T) -> String {
    // The fast write.
    let _fast = crate::pssz_checked::Undeclared::enter();
    match write_text::<F, T>(value) {
        Ok(text) => text,
        Err(e) => panic!("{}: the value has no text: {e}", F::NAME),
    }
}

/// Appends the text of `value` to `out`. On error `out` is left as it was.
///
/// The text is written straight into `out`: capacity the caller reserved is
/// used before any is allocated (text that fits it never reallocates), and
/// past it the buffer grows by doubling. No second buffer is made.
pub fn encode_into<F: JsonFormat, T: JsonText>(value: &T, out: &mut String) -> Result<()> {
    // The fast write, as `encode`: an error is a value that has no text.
    let _fast = crate::pssz_checked::Undeclared::enter();
    append_string::<F, T>(value, out)
}

/// The validated write, appended: [`try_encode`] into `out`. On error `out`
/// is left as it was.
pub fn try_encode_into<F: JsonFormat, T: JsonText>(value: &T, out: &mut String) -> Result<()> {
    let _validating = crate::pssz_checked::Declared::enter();
    append_string::<F, T>(value, out)
}

fn append_string<F: JsonFormat, T: JsonText>(value: &T, out: &mut String) -> Result<()> {
    let start = out.len();
    let mut bytes = std::mem::take(out).into_bytes();
    let result = append_text::<F, T>(value, &mut bytes);
    // The tail is checked, as `try_encode` checks all of its text; an error
    // has already removed it.
    assert!(
        std::str::from_utf8(&bytes[start..]).is_ok(),
        "the JSON writer emits UTF-8"
    );
    // SAFETY: `bytes[..start]` came out of a `String` and `bytes[start..]`
    // has just been validated; both end on a character boundary, so the
    // whole is UTF-8.
    *out = unsafe { String::from_utf8_unchecked(bytes) };
    result.map(|_| ())
}

/// Appends the text of `value` to `out`, in place, and returns its length.
/// On error `out` is left as it was.
fn append_text<F: JsonFormat, T: JsonText>(value: &T, out: &mut Vec<u8>) -> Result<usize> {
    let start = out.len();
    let mut w = Writer::appending(std::mem::take(out), F::IJSON);
    let result = value.write_json(&mut w);
    *out = w.into_bytes();
    match result {
        Ok(()) => Ok(out.len() - start),
        Err(e) => {
            out.truncate(start);
            Err(e)
        }
    }
}

/// Reads `text` as exactly one `T`. The reader checks as it reads.
pub fn decode<F: JsonFormat, T: JsonText>(text: impl AsRef<[u8]>) -> Result<T> {
    // A validating read validates whatever the caller is in the middle of.
    let _validating = crate::pssz_checked::Declared::enter();
    read_text::<F, T>(text.as_ref())
}

// The read, in whatever mode the thread is in.
fn read_text<F: JsonFormat, T: JsonText>(text: &[u8]) -> Result<T> {
    let mut r = Reader::new(text, F::IJSON);
    if text.is_empty() {
        return Err(r.err("json: empty input"));
    }
    let value = T::read_json(&mut r)?;
    r.ws();
    if r.pos != text.len() {
        return Err(r.err("json: trailing bytes after value"));
    }
    Ok(value)
}

/// Checks that `text` is exactly one `T`. Accepts what [`decode`] accepts.
pub fn validate<F: JsonFormat, T: JsonText>(text: impl AsRef<[u8]>) -> Result<()> {
    decode::<F, T>(text).map(|_| ())
}

/// Length in bytes of the text of `value`. Panics when the value has no
/// text, like [`encode`].
#[track_caller]
pub fn size_of<F: JsonFormat, T: JsonText>(value: &T) -> usize {
    encode::<F, T>(value).len()
}

macro_rules! helper_names {
    ($tag:ty, $name:literal, $cpp:literal, $to:ident, $try_to:ident, $to_into:ident, $try_to_into:ident, $from:ident,
     $from_trusted:ident, $validate:ident, $size:ident) => {
        #[doc = concat!("The ", $name, " text of `value` (C++ `psio::", $cpp, "`): the FAST write. The value is not held against what its type declares (bounds, order, invariants). Panics when the value has no text at all (NaN, an infinity, non-UTF-8 raw bytes); [`", stringify!($try_to), "`] is the validated write.")]
        #[track_caller]
        pub fn $to<T: JsonText>(value: &T) -> String {
            encode::<$tag, T>(value)
        }
        #[doc = concat!("The VALIDATED write (C++ `psio::", stringify!($to), "_checked(v)`): the text, or why a validating reader would refuse it — a member outside its bound or order, a violated invariant — or why the value has no text.")]
        pub fn $try_to<T: JsonText>(value: &T) -> Result<String> {
            try_encode::<$tag, T>(value)
        }
        #[doc = concat!("The validated write, appended: [`", stringify!($try_to), "`] into `out`. On error `out` is left as it was.")]
        pub fn $try_to_into<T: JsonText>(value: &T, out: &mut String) -> Result<()> {
            try_encode_into::<$tag, T>(value, out)
        }
        #[doc = concat!("Appends the ", $name, " text of `value` to `out`: the fast write, as [`", stringify!($to), "`]. An error is a value that has no text; `out` is left as it was.")]
        pub fn $to_into<T: JsonText>(value: &T, out: &mut String) -> Result<()> {
            encode_into::<$tag, T>(value, out)
        }
        #[doc = concat!("The checked decode: reads ", $name, " `text` as exactly one `T`. A missing member is an error unless it is an `Option`, which is `None`.")]
        pub fn $from<T: JsonText>(text: impl AsRef<[u8]>) -> Result<T> {
            decode::<$tag, T>(text)
        }
        #[doc = concat!("The TRUSTED read: ", $name, " text the caller vouches for. The text is parsed as strictly as ever (text that is not the ", $name, " of a `T` causes a panic), and the value is not held against what `T` declares (bounds, order, invariants).")]
        #[track_caller]
        pub fn $from_trusted<T: JsonText>(text: impl AsRef<[u8]>) -> T {
            let _trusted = crate::pssz_checked::Undeclared::enter();
            match read_text::<$tag, T>(text.as_ref()) {
                Ok(value) => value,
                Err(e) => panic!(concat!(stringify!($from_trusted), ": the text is not valid: {}"), e),
            }
        }
        #[doc = concat!("Whether `text` is exactly one `T` in ", $name, ". Never panics. Accepts what [`", stringify!($from), "`] accepts.")]
        pub fn $validate<T: JsonText>(text: impl AsRef<[u8]>) -> Result<()> {
            validate::<$tag, T>(text)
        }
        #[doc = concat!("Length in bytes of the ", $name, " text of `value`. Panics like [`", stringify!($to), "`].")]
        #[track_caller]
        pub fn $size<T: JsonText>(value: &T) -> usize {
            size_of::<$tag, T>(value)
        }
    };
}
helper_names!(Json, "JSON", "to_json(v)", to_json, try_to_json, to_json_into, try_to_json_into, from_json, from_json_trusted, validate_json, json_size);
helper_names!(IJson, "I-JSON", "to_ijson(v)", to_ijson, try_to_ijson, to_ijson_into, try_to_ijson_into, from_ijson, from_ijson_trusted, validate_ijson, ijson_size);

/// One `T` as JSON text, owned (C++ `psio::json_buffer<T>`).
pub type JsonBuffer<T> = crate::Buffer<T, Json>;
/// One `T` as JSON text, immutable and shared (C++ `psio::shared_json<T>`).
pub type SharedJson<T> = crate::Shared<T, Json>;
/// One `T` as I-JSON text, owned (C++ `psio::ijson_buffer<T>`).
pub type IJsonBuffer<T> = crate::Buffer<T, IJson>;
/// One `T` as I-JSON text, immutable and shared (C++ `psio::shared_ijson<T>`).
pub type SharedIJson<T> = crate::Shared<T, IJson>;

macro_rules! format_impls {
    ($tag:ty) => {
        impl<T: JsonText> Encode<$tag> for T {
            fn encode_into(&self, out: &mut Vec<u8>) -> Result<usize> {
                // In place: the capacity of `out` is used, no second buffer.
                let _validating = crate::pssz_checked::Declared::enter();
                append_text::<$tag, T>(self, out)
            }
        }
        impl<T: JsonText> Decode<$tag> for T {
            fn decode(bytes: &[u8]) -> Result<Self> {
                decode::<$tag, T>(bytes)
            }
        }
        impl<T: JsonText> Validate<$tag> for T {
            fn validate<P: ValidationPolicy>(bytes: &[u8], _policy: &P) -> Result<()> {
                validate::<$tag, T>(bytes)
            }
        }
    };
}
format_impls!(Json);
format_impls!(IJson);

mod value;
pub use value::{JsonNumber, JsonValue};

/// The kinds of JSON value a type may be written as, in either format, as a
/// bit set ([`JsonText::JSON_KINDS`]; docs/json.md "Untagged variants").
pub mod kinds {
    pub const NULL: u8 = 1;
    pub const BOOLEAN: u8 = 2;
    pub const NUMBER: u8 = 4;
    pub const STRING: u8 = 8;
    pub const ARRAY: u8 = 16;
    pub const OBJECT: u8 = 32;
    /// Every kind: also what a type that does not say is taken to be.
    pub const ANY: u8 = 63;
}

/// The bit of a kind in a [`JsonText::JSON_KINDS`] set.
pub const fn kind_bit(kind: JsonKind) -> u8 {
    match kind {
        JsonKind::Object => kinds::OBJECT,
        JsonKind::Array => kinds::ARRAY,
        JsonKind::String => kinds::STRING,
        JsonKind::Number => kinds::NUMBER,
        JsonKind::Bool => kinds::BOOLEAN,
        JsonKind::Null => kinds::NULL,
    }
}
/// The kinds an untagged enum is written as: its alternatives', which must
/// not overlap (used by the `JsonText` derive; evaluated when the type is
/// compiled).
///
/// ```
/// use psio::JsonText;
///
/// #[derive(Debug, PartialEq, JsonText)]
/// #[psio(untagged)] // or #[serde(untagged)]
/// enum Extra { One(String), Many(Vec<String>) }
///
/// assert_eq!(psio::to_ijson(&Extra::Many(vec!["a".into()])), r#"["a"]"#);
/// assert_eq!(psio::from_ijson::<Extra>(r#""s""#).unwrap(), Extra::One("s".into()));
/// ```
///
/// Two alternatives written as the same kind of JSON value do not compile:
///
/// ```compile_fail
/// #[derive(psio::JsonText)]
/// #[psio(untagged)]
/// enum Ambiguous { Int(i32), Float(f64) }
/// ```
///
/// (a 64-bit integer may be a string; bytes may be a string)
///
/// ```compile_fail
/// #[derive(psio::JsonText)]
/// #[serde(untagged)]
/// enum Ambiguous { Wide(u64), Text(String) }
/// ```
///
/// A field-less enum has no alternatives to leave bare:
///
/// ```compile_fail
/// #[derive(psio::JsonText)]
/// #[psio(untagged)]
/// #[repr(u8)]
/// enum Color { Red, Green }
/// ```
pub const fn untagged_kinds(alternatives: &[u8]) -> u8 {
    let mut all = 0;
    let mut i = 0;
    while i < alternatives.len() {
        if all & alternatives[i] != 0 {
            panic!(
                "the alternatives of an untagged enum must be told apart by the kind of JSON value each is written as"
            );
        }
        all |= alternatives[i];
        i += 1;
    }
    all
}

/// Reads one value of any shape from `c` and writes its compact text to `w`
/// (C++ `psio::detail::json_impl::capture_value`): no whitespace, strings
/// re-escaped by the writer's rule, a number as its token, members in order
/// (a repeated key repeated). The value is checked exactly as a skipped one
/// is, and `w`'s format decides whether a Unicode noncharacter is written.
pub fn capture_value<C: JsonCursor>(c: &mut C, w: &mut Writer) -> Result<()> {
    match c.kind()? {
        JsonKind::Object => {
            c.begin_object()?;
            w.raw("{");
            let mut first = true;
            loop {
                match c.next_field()? {
                    None => break,
                    Some(key) => w.key(&key, first)?,
                }
                first = false;
                capture_value(c, w)?;
            }
            w.raw("}");
        }
        JsonKind::Array => {
            c.begin_array()?;
            w.raw("[");
            let mut first = true;
            while c.next_element()? {
                if !first {
                    w.raw(",");
                }
                first = false;
                capture_value(c, w)?;
            }
            w.raw("]");
        }
        JsonKind::String => {
            let (text, _) = c.string()?;
            w.string(text.as_bytes())?;
        }
        JsonKind::Number => {
            let token = c.number()?;
            w.raw(token);
        }
        JsonKind::Bool => w.raw(if c.boolean()? { "true" } else { "false" }),
        JsonKind::Null => {
            c.null()?;
            w.raw("null");
        }
    }
    Ok(())
}

/// The compact `Json` text of the one JSON value `text` holds (under the
/// string rule of `ijson`), or why it is not one.
pub fn compact_json(text: &[u8], ijson: bool) -> Result<String> {
    let mut cursor = TextCursor::new(text, ijson)?;
    let mut w = Writer::new(ijson);
    capture_value(&mut cursor, &mut w)?;
    cursor.finish()?;
    // The writer only emits validated UTF-8.
    String::from_utf8(w.into_bytes()).map_err(|_| cursor.error("json: string is not valid UTF-8"))
}

// ── Writer ──────────────────────────────────────────────────────────────────

/// Output sink of [`JsonText::write_json`].
pub struct Writer {
    out: Vec<u8>,
    /// Where the text starts in `out` (a sink that appends).
    start: usize,
    ijson: bool,
    form: IntForm,
}

const HEX_LOWER: &[u8; 16] = b"0123456789abcdef";

fn is_noncharacter(cp: u32) -> bool {
    (0xFDD0..=0xFDEF).contains(&cp) || (cp & 0xFFFE) == 0xFFFE
}

impl Writer {
    /// An empty sink for `IJson` (`ijson`) or `Json` text.
    pub fn new(ijson: bool) -> Self {
        Self { out: Vec::new(), start: 0, ijson, form: IntForm::Natural }
    }
    /// A sink that appends to `out`, in its buffer: the capacity `out` has
    /// is used before any is allocated. [`Writer::into_bytes`] returns the
    /// buffer, the text after what it held.
    pub fn appending(out: Vec<u8>, ijson: bool) -> Self {
        Self { start: out.len(), out, ijson, form: IntForm::Natural }
    }
    /// The buffer: the text written so far (UTF-8), after what an appending
    /// sink held.
    pub fn into_bytes(self) -> Vec<u8> {
        self.out
    }
    pub fn is_ijson(&self) -> bool {
        self.ijson
    }
    fn format(&self) -> &'static str {
        if self.ijson { "ijson" } else { "json" }
    }
    pub fn err(&self, what: impl Into<Cow<'static, str>>) -> JsonTextError {
        JsonTextError { what: what.into(), offset: self.out.len() - self.start, format: self.format() }
    }
    /// Appends text that is already JSON (punctuation, keywords).
    pub fn raw(&mut self, text: &str) {
        self.out.extend_from_slice(text.as_bytes());
    }
    /// Sets the integer form for a record member and returns the previous
    /// one for [`Writer::restore_form`].
    pub fn member_form(&mut self, json: IntForm, ijson: IntForm) -> IntForm {
        std::mem::replace(&mut self.form, if self.ijson { ijson } else { json })
    }
    pub fn restore_form(&mut self, saved: IntForm) {
        self.form = saved;
    }
    /// `"name":` preceded by a comma unless `first`.
    pub fn key(&mut self, name: &str, first: bool) -> Result<()> {
        if !first {
            self.out.push(b',');
        }
        self.string(name.as_bytes())?;
        self.out.push(b':');
        Ok(())
    }

    /// `wide`: quoted under natural IJson: a 64-bit integer whose magnitude
    /// exceeds 2^53 - 1, or any 128- / 256-bit integer.
    fn integer(&mut self, digits: std::fmt::Arguments<'_>, wide: bool) {
        use std::io::Write;
        let quote = match self.form {
            IntForm::String => true,
            IntForm::Number => false,
            IntForm::Natural => self.ijson && wide,
        };
        if quote {
            self.out.push(b'"');
        }
        write!(self.out, "{}", digits).expect("writing to a Vec cannot fail");
        if quote {
            self.out.push(b'"');
        }
    }

    /// Writes bytes as a quoted, escaped JSON string. They must be UTF-8;
    /// IJson also refuses Unicode noncharacters.
    pub fn string(&mut self, bytes: &[u8]) -> Result<()> {
        let text = std::str::from_utf8(bytes)
            .map_err(|_| self.err("json: string is not valid UTF-8"))?;
        if self.ijson && text.chars().any(|c| is_noncharacter(c as u32)) {
            return Err(self.err("ijson: string holds a Unicode noncharacter"));
        }
        self.out.push(b'"');
        let mut run = 0;
        for (i, &c) in bytes.iter().enumerate() {
            if c >= 0x20 && c != b'"' && c != b'\\' {
                continue;
            }
            self.out.extend_from_slice(&bytes[run..i]);
            run = i + 1;
            match c {
                b'"' => self.out.extend_from_slice(b"\\\""),
                b'\\' => self.out.extend_from_slice(b"\\\\"),
                b'\n' => self.out.extend_from_slice(b"\\n"),
                b'\r' => self.out.extend_from_slice(b"\\r"),
                b'\t' => self.out.extend_from_slice(b"\\t"),
                _ => {
                    let esc = [
                        b'\\',
                        b'u',
                        b'0',
                        b'0',
                        HEX_LOWER[(c >> 4) as usize],
                        HEX_LOWER[(c & 15) as usize],
                    ];
                    self.out.extend_from_slice(&esc);
                }
            }
        }
        self.out.extend_from_slice(&bytes[run..]);
        self.out.push(b'"');
        Ok(())
    }

    fn hex(&mut self, bytes: &[u8]) {
        self.out.reserve(bytes.len() * 2 + 2);
        self.out.push(b'"');
        crate::hex::encode_upper(bytes, &mut self.out);
        self.out.push(b'"');
    }

    /// Shortest round-trip decimal in the layout of C++ `std::to_chars`:
    /// fixed or scientific, whichever is shorter (fixed on a tie), with a
    /// signed exponent of at least two digits.
    ///
    /// `shortest` is the Ryu text of the value (`ryu::Buffer`): the digit
    /// generation `std::to_chars` uses, so the digits — including the
    /// choice between two equally short candidates — are the same. Rust's
    /// own `{:e}` picks the other candidate on some ties (2^-24 as f64).
    /// Only the layout differs, and is redone here.
    fn float(&mut self, shortest: &str, exact_integer: impl FnOnce() -> String) {
        let (sign, body) = match shortest.strip_prefix('-') {
            Some(rest) => ("-", rest),
            None => ("", shortest),
        };
        let (mantissa, written_exponent) = match body.split_once('e') {
            Some((m, e)) => (m, e.parse::<i32>().expect("ryu exponent")),
            None => (body, 0),
        };
        let point = mantissa.find('.').unwrap_or(mantissa.len()) as i32;
        let all: String = mantissa.chars().filter(|c| *c != '.').collect();
        let leading = all.len() - all.trim_start_matches('0').len();
        let digits = all.trim_start_matches('0').trim_end_matches('0');
        if digits.is_empty() {
            // Zero (ryu writes "0.0" / "-0.0").
            self.out.extend_from_slice(sign.as_bytes());
            self.out.push(b'0');
            return;
        }
        // Decimal exponent of the first significant digit.
        let exponent = point - 1 - leading as i32 + written_exponent;
        let n = digits.len() as i32;
        let exp_digits = if exponent.abs() >= 100 { 3 } else { 2 };
        let scientific_len = n + i32::from(n > 1) + 2 + exp_digits;
        let fixed_len = if exponent < 0 {
            1 - exponent + n
        } else if exponent >= n - 1 {
            exponent + 1
        } else {
            n + 1
        };
        self.out.extend_from_slice(sign.as_bytes());
        if fixed_len <= scientific_len {
            if exponent < 0 {
                self.out.extend_from_slice(b"0.");
                self.out.extend(std::iter::repeat(b'0').take((-exponent - 1) as usize));
                self.out.extend_from_slice(digits.as_bytes());
            } else if exponent >= n - 1 {
                // An integer: every digit of its exact value.
                self.out.extend_from_slice(exact_integer().as_bytes());
            } else {
                let point = (exponent + 1) as usize;
                self.out.extend_from_slice(digits[..point].as_bytes());
                self.out.push(b'.');
                self.out.extend_from_slice(digits[point..].as_bytes());
            }
        } else {
            self.out.extend_from_slice(digits[..1].as_bytes());
            if n > 1 {
                self.out.push(b'.');
                self.out.extend_from_slice(digits[1..].as_bytes());
            }
            self.out.push(b'e');
            self.out.push(if exponent < 0 { b'-' } else { b'+' });
            let magnitude = exponent.abs();
            if magnitude < 10 {
                self.out.push(b'0');
            }
            self.out.extend_from_slice(magnitude.to_string().as_bytes());
        }
    }
}

// ── Reader ──────────────────────────────────────────────────────────────────

/// Input cursor of [`JsonText::read_json`].
pub struct Reader<'a> {
    src: &'a [u8],
    pos: usize,
    ijson: bool,
    form: IntForm,
    depth: usize,
}

use crate::hex::digit as hex_digit;

impl<'a> Reader<'a> {
    fn new(src: &'a [u8], ijson: bool) -> Self {
        Self { src, pos: 0, ijson, form: IntForm::Natural, depth: 0 }
    }
    pub fn is_ijson(&self) -> bool {
        self.ijson
    }
    pub fn position(&self) -> usize {
        self.pos
    }
    pub fn err(&self, what: impl Into<Cow<'static, str>>) -> JsonTextError {
        self.err_at(self.pos, what)
    }
    pub fn err_at(&self, offset: usize, what: impl Into<Cow<'static, str>>) -> JsonTextError {
        JsonTextError { what: what.into(), offset, format: if self.ijson { "ijson" } else { "json" } }
    }
    fn peek(&self) -> Option<u8> {
        self.src.get(self.pos).copied()
    }
    /// The kind of the next value; nothing but whitespace is consumed.
    pub fn kind(&mut self) -> Result<JsonKind> {
        self.ws();
        match self.peek() {
            None => Err(self.err("json: unexpected end of input")),
            Some(b'{') => Ok(JsonKind::Object),
            Some(b'[') => Ok(JsonKind::Array),
            Some(b'"') => Ok(JsonKind::String),
            Some(b't' | b'f') => Ok(JsonKind::Bool),
            Some(b'n') => Ok(JsonKind::Null),
            Some(b'-' | b'0'..=b'9') => Ok(JsonKind::Number),
            Some(_) => Err(self.err("json: unexpected character at value start")),
        }
    }
    pub fn ws(&mut self) {
        while let Some(b' ' | b'\t' | b'\n' | b'\r') = self.peek() {
            self.pos += 1;
        }
    }
    /// Skips whitespace, then consumes `c` if it is next.
    pub fn take(&mut self, c: u8) -> bool {
        self.ws();
        if self.peek() == Some(c) {
            self.pos += 1;
            true
        } else {
            false
        }
    }
    fn keyword(&mut self, word: &str) -> bool {
        if self.src[self.pos..].starts_with(word.as_bytes()) {
            self.pos += word.len();
            true
        } else {
            false
        }
    }
    /// Sets the integer form for a record member; see [`Writer::member_form`].
    pub fn member_form(&mut self, json: IntForm, ijson: IntForm) -> IntForm {
        std::mem::replace(&mut self.form, if self.ijson { ijson } else { json })
    }
    pub fn restore_form(&mut self, saved: IntForm) {
        self.form = saved;
    }
    /// Skips whitespace and says whether `c` is next, without consuming it.
    pub fn next_is(&mut self, c: u8) -> bool {
        self.ws();
        self.peek() == Some(c)
    }

    /// Opens an array or object and descends one level.
    pub fn begin(&mut self, open: u8, expected: &'static str) -> Result<()> {
        if !self.take(open) {
            return Err(self.err(expected));
        }
        if self.depth >= MAX_DEPTH {
            return Err(self.err("json: max depth exceeded"));
        }
        self.depth += 1;
        Ok(())
    }
    /// Leaves the array or object opened by [`Reader::begin`].
    pub fn end(&mut self) {
        self.depth -= 1;
    }
    /// Positions at element `index` of a fixed-arity array.
    pub fn element(&mut self, index: usize) -> Result<()> {
        if index > 0 {
            if !self.take(b',') {
                return Err(self.err("json: array has too few elements"));
            }
        } else {
            self.ws();
            if self.peek() == Some(b']') {
                return Err(self.err("json: array has too few elements"));
            }
        }
        Ok(())
    }
    /// Closes a fixed-arity array after its last element.
    pub fn close_array(&mut self) -> Result<()> {
        if !self.take(b']') {
            return Err(self.err("json: array has too many elements or is unterminated"));
        }
        self.end();
        Ok(())
    }
    pub fn expect(&mut self, c: u8, what: &'static str) -> Result<()> {
        if self.take(c) { Ok(()) } else { Err(self.err(what)) }
    }
    /// True when the next value is `null` (consumed).
    pub fn null(&mut self) -> Result<bool> {
        self.ws();
        if self.peek() != Some(b'n') {
            return Ok(false);
        }
        if self.keyword("null") { Ok(true) } else { Err(self.err("json: malformed keyword")) }
    }

    fn hex4(&mut self) -> Result<u32> {
        if self.src.len() - self.pos < 4 {
            return Err(self.err("json: truncated unicode escape"));
        }
        let mut v = 0u32;
        for k in 0..4 {
            let d = hex_digit(self.src[self.pos + k])
                .ok_or_else(|| self.err("json: bad hex digit in unicode escape"))?;
            v = (v << 4) | u32::from(d);
        }
        self.pos += 4;
        Ok(v)
    }

    /// Reads a JSON string at the cursor (after whitespace). Borrows the
    /// input when the string has no escapes.
    pub fn string(&mut self) -> Result<Cow<'a, str>> {
        self.ws();
        self.string_here(self.ijson)
    }

    fn string_here(&mut self, nonchar: bool) -> Result<Cow<'a, str>> {
        if self.peek() != Some(b'"') {
            return Err(self.err("json: expected a string"));
        }
        self.pos += 1;
        let src = self.src;
        let start = self.pos;
        let mut run = start;
        let mut owned: Option<String> = None;
        // Validates src[run..end] as UTF-8 text and returns it.
        let check = |this: &Self, run: usize, end: usize| -> Result<&'a str> {
            let text = std::str::from_utf8(&src[run..end]).map_err(|e| {
                this.err_at(run + e.valid_up_to(), "json: string is not valid UTF-8")
            })?;
            if nonchar && text.chars().any(|c| is_noncharacter(c as u32)) {
                return Err(this.err_at(run, "ijson: Unicode noncharacter in string"));
            }
            Ok(text)
        };
        loop {
            let Some(c) = self.peek() else {
                return Err(self.err("json: unterminated string"));
            };
            match c {
                b'"' => {
                    let tail = check(self, run, self.pos)?;
                    self.pos += 1;
                    return Ok(match owned {
                        None => Cow::Borrowed(tail),
                        Some(mut s) => {
                            s.push_str(tail);
                            Cow::Owned(s)
                        }
                    });
                }
                b'\\' => {
                    let head = check(self, run, self.pos)?;
                    let out = owned.get_or_insert_with(String::new);
                    out.push_str(head);
                    if self.src.len() - self.pos < 2 {
                        return Err(self.err("json: trailing backslash in string"));
                    }
                    let e = self.src[self.pos + 1];
                    self.pos += 2;
                    let cp = match e {
                        b'"' => '"' as u32,
                        b'\\' => '\\' as u32,
                        b'/' => '/' as u32,
                        b'b' => 8,
                        b'f' => 12,
                        b'n' => 10,
                        b'r' => 13,
                        b't' => 9,
                        b'u' => {
                            let mut cp = self.hex4()?;
                            if (0xDC00..=0xDFFF).contains(&cp) {
                                return Err(self.err("json: unpaired surrogate escape"));
                            }
                            if (0xD800..=0xDBFF).contains(&cp) {
                                if !self.src[self.pos..].starts_with(b"\\u") {
                                    return Err(self.err("json: unpaired surrogate escape"));
                                }
                                self.pos += 2;
                                let lo = self.hex4()?;
                                if !(0xDC00..=0xDFFF).contains(&lo) {
                                    return Err(self.err("json: unpaired surrogate escape"));
                                }
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            }
                            if nonchar && is_noncharacter(cp) {
                                return Err(self.err("ijson: Unicode noncharacter in string"));
                            }
                            cp
                        }
                        _ => {
                            self.pos -= 1;
                            return Err(self.err("json: invalid escape character in string"));
                        }
                    };
                    let ch = char::from_u32(cp).expect("surrogates were excluded");
                    owned.as_mut().expect("set above").push(ch);
                    run = self.pos;
                }
                0..=0x1f => return Err(self.err("json: unescaped control character in string")),
                _ => self.pos += 1,
            }
        }
    }

    /// Reads an object key and its colon.
    pub fn key(&mut self) -> Result<Cow<'a, str>> {
        self.ws();
        let key = self.string_here(self.ijson)?;
        if !self.take(b':') {
            return Err(self.err("json: expected ':' after object key"));
        }
        Ok(key)
    }

    /// After a member: true when the object closed, false after a comma.
    pub fn object_next(&mut self) -> Result<bool> {
        if self.take(b'}') {
            return Ok(true);
        }
        if self.take(b',') {
            return Ok(false);
        }
        Err(self.err("json: expected ',' or '}' in object"))
    }

    /// Skips the value of a key that matched no member.
    pub fn skip_unknown(&mut self) -> Result<()> {
        self.skip_value()
    }

    /// Closes an object opened by [`Reader::begin`].
    pub fn end_object(&mut self) {
        self.end();
    }

    fn skip_number(&mut self) -> Result<()> {
        let src = self.src;
        let mut p = self.pos;
        let digit = |p: usize| matches!(src.get(p), Some(b'0'..=b'9'));
        if src.get(p) == Some(&b'-') {
            p += 1;
        }
        match src.get(p) {
            None => return Err(self.err("json: truncated number")),
            Some(b'0') => p += 1,
            Some(b'1'..=b'9') => {
                while digit(p) {
                    p += 1;
                }
            }
            _ => return Err(self.err("json: number missing integer part")),
        }
        if src.get(p) == Some(&b'.') {
            p += 1;
            let frac = p;
            while digit(p) {
                p += 1;
            }
            if p == frac {
                return Err(self.err("json: number missing fractional digits"));
            }
        }
        if matches!(src.get(p), Some(b'e' | b'E')) {
            p += 1;
            if matches!(src.get(p), Some(b'+' | b'-')) {
                p += 1;
            }
            let exp = p;
            while digit(p) {
                p += 1;
            }
            if p == exp {
                return Err(self.err("json: number missing exponent digits"));
            }
        }
        self.pos = p;
        Ok(())
    }

    /// Skips one well-formed JSON value of any shape.
    fn skip_value(&mut self) -> Result<()> {
        self.ws();
        // Strings inside a skipped value still follow the format's rules
        // (noncharacters are not I-JSON).
        let unique = self.ijson;
        match self.peek() {
            None => Err(self.err("json: unexpected end of input")),
            Some(b'{') => {
                self.begin(b'{', "json: expected '{'")?;
                if !self.take(b'}') {
                    loop {
                        self.key()?;
                        self.skip_value()?;
                        if self.object_next()? {
                            break;
                        }
                    }
                }
                self.end();
                Ok(())
            }
            Some(b'[') => {
                self.begin(b'[', "json: expected '['")?;
                if !self.take(b']') {
                    loop {
                        self.skip_value()?;
                        if self.take(b']') {
                            break;
                        }
                        if !self.take(b',') {
                            return Err(self.err("json: expected ',' or ']' in array"));
                        }
                    }
                }
                self.end();
                Ok(())
            }
            Some(b'"') => self.string_here(unique).map(|_| ()),
            Some(b't') => self.keyword("true").then_some(()).ok_or_else(|| self.err("json: malformed keyword")),
            Some(b'f') => self.keyword("false").then_some(()).ok_or_else(|| self.err("json: malformed keyword")),
            Some(b'n') => self.keyword("null").then_some(()).ok_or_else(|| self.err("json: malformed keyword")),
            Some(b'-' | b'0'..=b'9') => self.skip_number(),
            Some(_) => Err(self.err("json: unexpected character at value start")),
        }
    }

    /// The characters between the quotes of an integer string at the cursor
    /// (which is at the opening quote). An escape is not unescaped: its
    /// backslash is no digit, so the caller's parse rejects it.
    fn quoted_integer(&mut self) -> Result<&'a str> {
        let src = self.src;
        let begin = self.pos + 1;
        let mut q = begin;
        while q < src.len() && src[q] != b'"' {
            q += 1;
        }
        if q >= src.len() {
            return Err(self.err("json: unterminated string"));
        }
        let text = std::str::from_utf8(&src[begin..q])
            .map_err(|_| self.err("json: integer string is not a canonical in-range decimal"))?;
        self.pos = q + 1;
        Ok(text)
    }

    /// A bare integer token at the cursor: `-?[0-9]+`, with no fraction or
    /// exponent after it.
    fn bare_integer(&mut self) -> Result<&'a str> {
        let src = self.src;
        let begin = self.pos;
        let mut q = begin;
        if src.get(q) == Some(&b'-') {
            q += 1;
        }
        while matches!(src.get(q), Some(b'0'..=b'9')) {
            q += 1;
        }
        if q == begin {
            return Err(self.err("json: expected an integer"));
        }
        if matches!(src.get(q), Some(b'.' | b'e' | b'E')) {
            return Err(self.err("json: integer has a fraction or exponent"));
        }
        self.pos = q;
        // Only ASCII digits and '-' were accepted.
        Ok(std::str::from_utf8(&src[begin..q]).expect("ASCII"))
    }

    /// The characters of an integer at the cursor: either the digits between
    /// quotes or a bare number token, per the form in force.
    fn integer_token(&mut self, wide: bool) -> Result<(&'a str, bool)> {
        self.ws();
        let as_string = match self.form {
            IntForm::String => true,
            IntForm::Number => false,
            IntForm::Natural => self.ijson && wide,
        };
        match self.peek() {
            None => Err(self.err("json: unexpected end of input")),
            Some(b'"') => {
                if !as_string {
                    return Err(self.err("json: expected a number, found a string"));
                }
                Ok((self.quoted_integer()?, false))
            }
            Some(_) => {
                if as_string && self.form == IntForm::String {
                    return Err(self.err("json: expected an integer string"));
                }
                Ok((self.bare_integer()?, as_string))
            }
        }
    }

    /// The characters of a 128- or 256-bit integer at the cursor and whether
    /// they were quoted. Both forms are read unless one is forced.
    fn wide_integer_token(&mut self) -> Result<(&'a str, bool)> {
        self.ws();
        match self.peek() {
            None => Err(self.err("json: unexpected end of input")),
            Some(b'"') => {
                if self.form == IntForm::Number {
                    return Err(self.err("json: expected a number, found a string"));
                }
                Ok((self.quoted_integer()?, true))
            }
            Some(_) => {
                if self.form == IntForm::String {
                    return Err(self.err("json: expected an integer string"));
                }
                Ok((self.bare_integer()?, false))
            }
        }
    }
}

/// `0 | -?[1-9][0-9]*`
fn canonical_integer(text: &str, signed: bool) -> bool {
    let digits = match text.strip_prefix('-') {
        Some(rest) => {
            if !signed {
                return false;
            }
            rest
        }
        None => text,
    };
    if digits.is_empty() || !digits.bytes().all(|b| b.is_ascii_digit()) {
        return false;
    }
    if digits.starts_with('0') {
        return text == "0";
    }
    true
}

// ── Primitive implementations ───────────────────────────────────────────────

impl JsonText for bool {
    const JSON_KINDS: u8 = kinds::BOOLEAN;
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        w.raw(if *self { "true" } else { "false" });
        Ok(())
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        r.ws();
        if r.keyword("true") {
            Ok(true)
        } else if r.keyword("false") {
            Ok(false)
        } else {
            Err(r.err("json: expected true or false"))
        }
    }
}

macro_rules! integer {
    ($($ty:ty, $signed:expr;)*) => {$(
        impl JsonText for $ty {
            const JSON_KINDS: u8 =
                if ::core::mem::size_of::<$ty>() >= 8 { kinds::NUMBER | kinds::STRING } else { kinds::NUMBER };
            fn write_json(&self, w: &mut Writer) -> Result<()> {
                // A 64-bit integer is quoted under IJson only beyond the
                // range JavaScript holds exactly.
                w.integer(
                    format_args!("{}", self),
                    std::mem::size_of::<$ty>() >= 8
                        && (*self as i128).unsigned_abs() > SAFE_INTEGER as u128,
                );
                Ok(())
            }
            fn read_json(r: &mut Reader<'_>) -> Result<Self> {
                let at = r.pos;
                let (text, bare_wide) = r.integer_token(std::mem::size_of::<$ty>() >= 8)?;
                let value = if canonical_integer(text, $signed) { text.parse::<$ty>().ok() } else { None };
                let Some(value) = value else {
                    return Err(r.err_at(at, "json: integer is not a canonical in-range decimal"));
                };
                // Natural IJson: a bare number only where JavaScript is exact.
                if bare_wide && (value as i128).unsigned_abs() > SAFE_INTEGER as u128 {
                    return Err(r.err_at(
                        at,
                        "ijson: bare integer outside the safe range must be a string",
                    ));
                }
                Ok(value)
            }
        }
    )*};
}
integer! {
    i8, true; i16, true; i32, true; i64, true;
    u16, false; u32, false; u64, false;
}

// u8 is an integer on its own and a byte inside a sequence.
impl JsonText for u8 {
    const JSON_KINDS: u8 = kinds::NUMBER;
    // Bytes: an array of numbers under Json, a hex string under IJson.
    const JSON_SEQUENCE_KINDS: u8 = kinds::ARRAY | kinds::STRING;
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        w.integer(format_args!("{}", self), false);
        Ok(())
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        let at = r.pos;
        let (text, _) = r.integer_token(false)?;
        if canonical_integer(text, false) {
            if let Ok(v) = text.parse::<u8>() {
                return Ok(v);
            }
        }
        Err(r.err_at(at, "json: integer is not a canonical in-range decimal"))
    }
    fn write_json_slice(items: &[Self], w: &mut Writer) -> Result<()> {
        if w.ijson {
            w.hex(items);
            return Ok(());
        }
        w.out.push(b'[');
        for (i, item) in items.iter().enumerate() {
            if i > 0 {
                w.out.push(b',');
            }
            item.write_json(w)?;
        }
        w.out.push(b']');
        Ok(())
    }
    fn read_json_vec(r: &mut Reader<'_>) -> Result<Vec<Self>> {
        if r.ijson {
            return read_hex(r, None);
        }
        let mut out = Vec::new();
        r.begin(b'[', "json: expected '['")?;
        if !r.take(b']') {
            loop {
                out.push(u8::read_json(r)?);
                if r.take(b']') {
                    break;
                }
                if !r.take(b',') {
                    return Err(r.err("json: expected ',' or ']' in array"));
                }
            }
        }
        r.end();
        Ok(out)
    }
    fn read_json_array<const N: usize>(r: &mut Reader<'_>) -> Result<[Self; N]> {
        let bytes = if r.ijson {
            read_hex(r, Some(N))?
        } else {
            let mut out = Vec::with_capacity(N);
            r.begin(b'[', "json: expected '['")?;
            for i in 0..N {
                r.element(i)?;
                out.push(u8::read_json(r)?);
            }
            r.close_array()?;
            out
        };
        bytes.try_into().map_err(|_| r.err("json: array has the wrong length"))
    }
}

// ── 128- and 256-bit integers ───────────────────────────────────────────────
//
// Decimal in both formats: a bare number under Json, a string under IJson
// (always, so an amount has one form whatever its size). Read from a decimal
// string, from a bare number (IJson: |n| <= 2^53 - 1, the 64-bit rule) and
// from the older "0x<hex>" string; a forced form accepts that form only.

/// A 256-bit magnitude, least-significant limb first.
pub(crate) type Wide = [u64; 4];

/// The text of a wide integer of `bits` (128 or 256) bits as sign and
/// magnitude: decimal `0 | -?[1-9][0-9]*`, or the legacy `[-]0x<hex>` with at
/// least one and at most `bits / 4` digits of either case. The magnitude is
/// only known to fit 256 bits; the caller checks the type's range.
pub(crate) fn wide_parse(text: &str, bits: u32) -> Option<(bool, Wide)> {
    let (negative, body) = match text.strip_prefix('-') {
        Some(rest) => (true, rest),
        None => (false, text),
    };
    let mut value: Wide = [0; 4];
    if body.len() > 2 && (body.starts_with("0x") || body.starts_with("0X")) {
        let digits = &body.as_bytes()[2..];
        if digits.len() > (bits / 4) as usize {
            return None;
        }
        for &c in digits {
            let mut carry = u64::from(hex_digit(c)?);
            for limb in value.iter_mut() {
                let next = *limb >> 60;
                *limb = (*limb << 4) | carry;
                carry = next;
            }
        }
        return Some((negative, value));
    }
    if !canonical_integer(text, true) {
        return None;
    }
    for c in body.bytes() {
        let mut carry = u128::from(c - b'0');
        for limb in value.iter_mut() {
            let wide = u128::from(*limb) * 10 + carry;
            *limb = wide as u64;
            carry = wide >> 64;
        }
        if carry != 0 {
            return None;
        }
    }
    Some((negative, value))
}

pub(crate) fn wide_to_u128(negative: bool, value: Wide) -> Option<u128> {
    if negative || value[2] != 0 || value[3] != 0 {
        return None;
    }
    Some(u128::from(value[0]) | (u128::from(value[1]) << 64))
}

pub(crate) fn wide_to_i128(negative: bool, value: Wide) -> Option<i128> {
    let magnitude = wide_to_u128(false, value)?;
    const HALF: u128 = 1 << 127;
    if negative {
        (magnitude <= HALF).then(|| 0u128.wrapping_sub(magnitude) as i128)
    } else {
        (magnitude < HALF).then_some(magnitude as i128)
    }
}

pub(crate) fn wide_to_u256(negative: bool, value: Wide) -> Option<crate::pssz::Uint256> {
    (!negative).then_some(crate::pssz::Uint256 { limb: value })
}

/// |n| <= 2^53 - 1: what a bare I-JSON number may hold.
pub(crate) fn wide_is_safe(value: &Wide) -> bool {
    value[1] == 0 && value[2] == 0 && value[3] == 0 && value[0] <= SAFE_INTEGER
}

/// The decimal digits of a 256-bit value.
pub(crate) fn wide_decimal(mut value: Wide) -> String {
    const CHUNK: u128 = 10_000_000_000_000_000_000; // 10^19
    let mut chunks: Vec<u64> = Vec::new();
    while value != [0; 4] {
        let mut remainder = 0u128;
        for limb in value.iter_mut().rev() {
            let current = (remainder << 64) | u128::from(*limb);
            *limb = (current / CHUNK) as u64;
            remainder = current % CHUNK;
        }
        chunks.push(remainder as u64);
    }
    let Some(top) = chunks.pop() else { return "0".to_owned() };
    let mut out = top.to_string();
    for chunk in chunks.iter().rev() {
        out.push_str(&format!("{chunk:019}"));
    }
    out
}

fn read_wide<T>(
    r: &mut Reader<'_>,
    bits: u32,
    convert: impl FnOnce(bool, Wide) -> Option<T>,
) -> Result<T> {
    let at = r.pos;
    let (text, quoted) = r.wide_integer_token()?;
    let bad = if quoted {
        "json: integer string is not a canonical in-range decimal"
    } else {
        "json: integer is not a canonical in-range decimal"
    };
    let Some((negative, magnitude)) = wide_parse(text, bits) else {
        return Err(r.err_at(at, bad));
    };
    let Some(value) = convert(negative, magnitude) else {
        return Err(r.err_at(at, bad));
    };
    // Natural IJson: a bare number only where JavaScript is exact.
    if !quoted && r.ijson && r.form == IntForm::Natural && !wide_is_safe(&magnitude) {
        return Err(r.err_at(at, "ijson: bare integer outside the safe range must be a string"));
    }
    Ok(value)
}

impl JsonText for u128 {
    const JSON_KINDS: u8 = kinds::NUMBER | kinds::STRING;
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        w.integer(format_args!("{}", self), true);
        Ok(())
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        read_wide(r, 128, wide_to_u128)
    }
}

impl JsonText for i128 {
    const JSON_KINDS: u8 = kinds::NUMBER | kinds::STRING;
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        w.integer(format_args!("{}", self), true);
        Ok(())
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        read_wide(r, 128, wide_to_i128)
    }
}

/// C++ `psio::uint256`: 32 little-endian bytes on the wire, decimal in text.
impl JsonText for crate::pssz::Uint256 {
    const JSON_KINDS: u8 = kinds::NUMBER | kinds::STRING;
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        w.integer(format_args!("{}", wide_decimal(self.limb)), true);
        Ok(())
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        read_wide(r, 256, wide_to_u256)
    }
}

fn read_hex(r: &mut Reader<'_>, exact: Option<usize>) -> Result<Vec<u8>> {
    r.ws();
    if r.peek() != Some(b'"') {
        return Err(r.err("ijson: expected a hex string"));
    }
    let src = r.src;
    let begin = r.pos + 1;
    let q = begin + crate::hex::digit_run(&src[begin..]);
    if q >= src.len() {
        return Err(r.err("json: unterminated string"));
    }
    if src[q] != b'"' {
        return Err(r.err_at(q, "ijson: non-hex character in hex string"));
    }
    let digits = &src[begin..q];
    if digits.len() % 2 != 0 {
        return Err(r.err("ijson: odd-length hex string"));
    }
    if exact.is_some_and(|n| n != digits.len() / 2) {
        return Err(r.err("ijson: hex string has the wrong length"));
    }
    r.pos = q + 1;
    Ok(crate::hex::decode_vec(digits))
}

macro_rules! float {
    ($($ty:ty)*) => {$(
        impl JsonText for $ty {
            const JSON_KINDS: u8 = kinds::NUMBER;
            fn write_json(&self, w: &mut Writer) -> Result<()> {
                if !self.is_finite() {
                    // No JSON text exists for NaN or an infinity: both
                    // formats refuse, in every language.
                    return Err(w.err("json: non-finite float has no JSON text"));
                }
                let value = *self;
                let mut buffer = ryu::Buffer::new();
                w.float(buffer.format_finite(value), || {
                    format!("{}", value.abs() as f64 as u128)
                });
                Ok(())
            }
            fn read_json(r: &mut Reader<'_>) -> Result<Self> {
                r.ws();
                let begin = r.pos;
                if !matches!(r.peek(), Some(b'-' | b'0'..=b'9')) {
                    return Err(r.err("json: expected a number"));
                }
                r.skip_number()?;
                let token = std::str::from_utf8(&r.src[begin..r.pos]).expect("ASCII");
                match token.parse::<$ty>() {
                    Ok(v) if v.is_finite() => Ok(v),
                    _ => Err(r.err_at(begin, "json: number out of range")),
                }
            }
        }
    )*};
}
float!(f32 f64);

impl JsonText for String {
    const JSON_KINDS: u8 = kinds::STRING;
    const JSON_STRING: bool = true;
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        w.string(self.as_bytes())
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        r.string().map(Cow::into_owned)
    }
}

/// A raw string is text-shaped bytes: a JSON string when the bytes are
/// UTF-8, and an error to write otherwise (hex would hide the text, and
/// invalid UTF-8 is not JSON).
impl JsonText for RawString {
    const JSON_KINDS: u8 = kinds::STRING;
    const JSON_STRING: bool = true;
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        w.string(&self.0)
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        r.string().map(|s| RawString(s.into_owned().into_bytes()))
    }
}

// Bounded types: the text of the unbounded type, with the bound enforced on
// both sides. They pair with C++ `psio::utf8_string<N>`, a `std::string` /
// `std::vector` member annotated `length_bound{.max = N}`, and
// `psio::bounded<T, N>`.
impl<const N: usize> JsonText for crate::pssz::BoundedString<N> {
    const JSON_KINDS: u8 = kinds::STRING;
    const JSON_STRING: bool = true;
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        if self.0.len() > N && crate::pssz_checked::declared() {
            return Err(w.err("json: string exceeds its bound"));
        }
        w.string(self.0.as_bytes())
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        r.ws();
        let at = r.pos;
        let text = r.string()?;
        if text.len() > N && crate::pssz_checked::declared() {
            return Err(r.err_at(at, "json: string exceeds its bound"));
        }
        Ok(Self(text.into_owned()))
    }
}

impl<const N: usize> JsonText for crate::pssz::BoundedRawString<N> {
    const JSON_KINDS: u8 = kinds::STRING;
    const JSON_STRING: bool = true;
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        if self.0.len() > N && crate::pssz_checked::declared() {
            return Err(w.err("json: string exceeds its bound"));
        }
        w.string(&self.0)
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        r.ws();
        let at = r.pos;
        let text = r.string()?;
        if text.len() > N && crate::pssz_checked::declared() {
            return Err(r.err_at(at, "json: string exceeds its bound"));
        }
        Ok(Self(text.into_owned().into_bytes()))
    }
}

impl<T: JsonText, const N: usize> JsonText for crate::pssz::BoundedList<T, N> {
    const JSON_KINDS: u8 = T::JSON_SEQUENCE_KINDS;
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        if self.0.len() > N && crate::pssz_checked::declared() {
            return Err(w.err("json: array exceeds its bound"));
        }
        T::write_json_slice(&self.0, w)
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        r.ws();
        let at = r.pos;
        let items = T::read_json_vec(r)?;
        if items.len() > N && crate::pssz_checked::declared() {
            return Err(r.err_at(at, "json: array exceeds its bound"));
        }
        Ok(Self(items))
    }
}

impl<T: JsonText> JsonText for Vec<T> {
    const JSON_KINDS: u8 = T::JSON_SEQUENCE_KINDS;
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        T::write_json_slice(self, w)
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        T::read_json_vec(r)
    }
}

impl<T: JsonText, const N: usize> JsonText for [T; N] {
    const JSON_KINDS: u8 = T::JSON_SEQUENCE_KINDS;
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        T::write_json_slice(self, w)
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        T::read_json_array::<N>(r)
    }
}

impl<T: JsonText> JsonText for Option<T> {
    const JSON_KINDS: u8 = kinds::NULL | T::JSON_KINDS;
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        match self {
            Some(v) => v.write_json(w),
            None => {
                w.raw("null");
                Ok(())
            }
        }
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        if r.null()? { Ok(None) } else { T::read_json(r).map(Some) }
    }
    fn json_missing() -> Option<Self> {
        Some(None)
    }
}

impl<T: JsonText> JsonText for Box<T> {
    const JSON_KINDS: u8 = T::JSON_KINDS;
    const JSON_STRING: bool = T::JSON_STRING;
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        (**self).write_json(w)
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        T::read_json(r).map(Box::new)
    }
}

// Tuples are arrays of exactly their arity; elements use the format's
// natural integer form.
macro_rules! tuple {
    ($($name:ident $index:tt),*) => {
        impl<$($name: JsonText),*> JsonText for ($($name,)*) {
            const JSON_KINDS: u8 = kinds::ARRAY;
            #[allow(unused_variables, unused_mut)]
            fn write_json(&self, w: &mut Writer) -> Result<()> {
                let saved = w.member_form(IntForm::Natural, IntForm::Natural);
                w.raw("[");
                $(
                    if $index > 0 { w.raw(","); }
                    self.$index.write_json(w)?;
                )*
                w.raw("]");
                w.restore_form(saved);
                Ok(())
            }
            #[allow(unused_variables, unused_mut)]
            fn read_json(r: &mut Reader<'_>) -> Result<Self> {
                let saved = r.member_form(IntForm::Natural, IntForm::Natural);
                r.begin(b'[', "json: expected '['")?;
                let value = ($({
                    r.element($index)?;
                    $name::read_json(r)?
                },)*);
                r.close_array()?;
                r.restore_form(saved);
                Ok(value)
            }
        }
    };
}
tuple!();
tuple!(A 0);
tuple!(A 0, B 1);
tuple!(A 0, B 1, C 2);
tuple!(A 0, B 1, C 2, D 3);
tuple!(A 0, B 1, C 2, D 3, E 4);
tuple!(A 0, B 1, C 2, D 3, E 4, F 5);
tuple!(A 0, B 1, C 2, D 3, E 4, F 5, G 6);
tuple!(A 0, B 1, C 2, D 3, E 4, F 5, G 6, H 7);
tuple!(A 0, B 1, C 2, D 3, E 4, F 5, G 6, H 7, I 8);
tuple!(A 0, B 1, C 2, D 3, E 4, F 5, G 6, H 7, I 8, J 9);
tuple!(A 0, B 1, C 2, D 3, E 4, F 5, G 6, H 7, I 8, J 9, K 10);
tuple!(A 0, B 1, C 2, D 3, E 4, F 5, G 6, H 7, I 8, J 9, K 10, L 11);

// ── Maps and sets (docs/json.md, "Maps and sets") ───────────────────────────
//
// A set is an array of its elements. A map is an object when the text of its
// key type is a JSON string by type (`JsonText::JSON_STRING`), and an array
// of `[key, value]` pairs otherwise. Writers emit the canonical order
// (ascending key bytes); readers accept any order and reject a repeated key
// or element (two keys are the same when their key bytes are equal).

use crate::key::{key_bytes, ByKey, KeyPack};

fn duplicates<K: KeyPack>(keys: impl Iterator<Item = K>) -> bool {
    let mut bytes: Vec<Vec<u8>> = keys.map(|key| key_bytes(&key)).collect();
    bytes.sort();
    bytes.windows(2).any(|pair| pair[0] == pair[1])
}
fn sorted<I, K: KeyPack>(items: impl Iterator<Item = I>, key_of: impl Fn(&I) -> &K) -> Option<Vec<I>> {
    let mut keyed: Vec<(Vec<u8>, I)> = items.map(|item| (key_bytes(key_of(&item)), item)).collect();
    keyed.sort_by(|a, b| a.0.cmp(&b.0));
    if keyed.windows(2).any(|pair| pair[0].0 == pair[1].0) {
        return None;
    }
    Some(keyed.into_iter().map(|(_, item)| item).collect())
}

fn write_map<'a, K, V>(
    entries: impl Iterator<Item = (&'a K, &'a V)>,
    bound: usize,
    w: &mut Writer,
) -> Result<()>
where
    K: JsonText + KeyPack + 'a,
    V: JsonText + 'a,
{
    let Some(entries) = sorted(entries, |entry| entry.0) else {
        return Err(w.err("json: map has duplicate keys"));
    };
    if entries.len() > bound && crate::pssz_checked::declared() {
        return Err(w.err("json: array exceeds its bound"));
    }
    // An entry is a tuple: its key and value take the natural form.
    let saved = w.member_form(IntForm::Natural, IntForm::Natural);
    w.raw(if K::JSON_STRING { "{" } else { "[" });
    for (i, (key, value)) in entries.into_iter().enumerate() {
        if i > 0 {
            w.raw(",");
        }
        if K::JSON_STRING {
            key.write_json(w)?;
            w.raw(":");
            value.write_json(w)?;
        } else {
            w.raw("[");
            key.write_json(w)?;
            w.raw(",");
            value.write_json(w)?;
            w.raw("]");
        }
    }
    w.raw(if K::JSON_STRING { "}" } else { "]" });
    w.restore_form(saved);
    Ok(())
}
/// An object key is read exactly as the same text would be read as a string
/// of the key type: its bound, its text form, its invariant.
fn read_map<K: JsonText + KeyPack, V: JsonText>(r: &mut Reader<'_>, bound: usize) -> Result<Vec<(K, V)>> {
    r.ws();
    let at = r.pos;
    let saved = r.member_form(IntForm::Natural, IntForm::Natural);
    let mut entries: Vec<(K, V)> = Vec::new();
    if K::JSON_STRING {
        r.begin(b'{', "json: expected an object (a map with string keys)")?;
        if !r.take(b'}') {
            loop {
                r.ws();
                let key = K::read_json(r)?;
                r.expect(b':', "json: expected ':' after object key")?;
                entries.push((key, V::read_json(r)?));
                if r.object_next()? {
                    break;
                }
            }
        }
        r.end_object();
    } else {
        entries = <(K, V)>::read_json_vec(r)?;
    }
    r.restore_form(saved);
    if entries.len() > bound && crate::pssz_checked::declared() {
        return Err(r.err_at(at, "json: array exceeds its bound"));
    }
    if duplicates(entries.iter().map(|entry| &entry.0)) {
        return Err(r.err_at(at, "json: duplicate map key"));
    }
    Ok(entries)
}
/// A member's integer form reaches the elements of a set, as of a list. A
/// set of bytes is an array of numbers in both formats: not a byte string.
fn write_set<'a, K: JsonText + KeyPack + 'a>(
    elements: impl Iterator<Item = &'a K>,
    bound: usize,
    w: &mut Writer,
) -> Result<()> {
    let Some(elements) = sorted(elements, |element| *element) else {
        return Err(w.err("json: set has duplicate elements"));
    };
    if elements.len() > bound && crate::pssz_checked::declared() {
        return Err(w.err("json: array exceeds its bound"));
    }
    w.raw("[");
    for (i, element) in elements.into_iter().enumerate() {
        if i > 0 {
            w.raw(",");
        }
        element.write_json(w)?;
    }
    w.raw("]");
    Ok(())
}
fn read_set<K: JsonText + KeyPack>(r: &mut Reader<'_>, bound: usize) -> Result<Vec<K>> {
    r.ws();
    let at = r.pos;
    let mut elements = Vec::new();
    r.begin(b'[', "json: expected '['")?;
    if !r.take(b']') {
        loop {
            elements.push(K::read_json(r)?);
            if r.take(b']') {
                break;
            }
            if !r.take(b',') {
                return Err(r.err("json: expected ',' or ']' in array"));
            }
        }
    }
    r.end();
    if elements.len() > bound && crate::pssz_checked::declared() {
        return Err(r.err_at(at, "json: array exceeds its bound"));
    }
    if duplicates(elements.iter()) {
        return Err(r.err_at(at, "json: duplicate set element"));
    }
    Ok(elements)
}

macro_rules! json_map {
    ([$($kb:tt)*], [$($cg:tt)*], $ty:ty, $bound:expr, |$s:ident| $inner:expr, |$items:ident| $build:expr) => {
        impl<K: JsonText + KeyPack + $($kb)*, V: JsonText, $($cg)*> JsonText for $ty {
            const JSON_KINDS: u8 = kinds::OBJECT | kinds::ARRAY;
            fn write_json(&self, w: &mut Writer) -> Result<()> {
                let $s = self;
                write_map($inner.iter(), $bound, w)
            }
            fn read_json(r: &mut Reader<'_>) -> Result<Self> {
                r.ws();
                let at = r.pos;
                let $items = read_map::<K, V>(r, $bound)?;
                let count = $items.len();
                let out: Self = $build;
                let $s = &out;
                if $inner.len() != count {
                    return Err(r.err_at(at, "json: map container merged distinct keys"));
                }
                Ok(out)
            }
        }
    };
}
macro_rules! json_set {
    ([$($kb:tt)*], [$($cg:tt)*], $ty:ty, $bound:expr, |$s:ident| $inner:expr, |$items:ident| $build:expr) => {
        impl<K: JsonText + KeyPack + $($kb)*, $($cg)*> JsonText for $ty {
            const JSON_KINDS: u8 = kinds::ARRAY;
            fn write_json(&self, w: &mut Writer) -> Result<()> {
                let $s = self;
                write_set($inner.iter(), $bound, w)
            }
            fn read_json(r: &mut Reader<'_>) -> Result<Self> {
                r.ws();
                let at = r.pos;
                let $items = read_set::<K>(r, $bound)?;
                let count = $items.len();
                let out: Self = $build;
                let $s = &out;
                if $inner.len() != count {
                    return Err(r.err_at(at, "json: set container merged distinct elements"));
                }
                Ok(out)
            }
        }
    };
}
use crate::pssz::{BoundedMap, BoundedSet};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::hash::Hash;
json_map!([Ord], [], BTreeMap<K, V>, usize::MAX, |s| s, |items| items.into_iter().collect());
json_map!([Eq + Hash], [], HashMap<K, V>, usize::MAX, |s| s, |items| items.into_iter().collect());
json_map!([Ord], [const N: usize], BoundedMap<K, V, N>, N, |s| s.0,
          |items| BoundedMap(items.into_iter().collect()));
json_set!([Ord], [], BTreeSet<K>, usize::MAX, |s| s, |items| items.into_iter().collect());
json_set!([Eq + Hash], [], HashSet<K>, usize::MAX, |s| s, |items| items.into_iter().collect());
json_set!([Ord], [const N: usize], BoundedSet<K, N>, N, |s| s.0,
          |items| BoundedSet(items.into_iter().collect()));

impl<T: JsonText> JsonText for ByKey<T> {
    const JSON_KINDS: u8 = T::JSON_KINDS;
    const JSON_STRING: bool = T::JSON_STRING;
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        self.0.write_json(w)
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        T::read_json(r).map(ByKey)
    }
    fn json_missing() -> Option<Self> {
        T::json_missing().map(ByKey)
    }
}

// ── Support for #[derive(JsonText)] on enums ────────────────────────────────

/// Reads a field-less enum as the value of its `#[repr]` type `T`. Accepted
/// in both formats: the enumerator's name (`"active"`), the number (`2`, by
/// the integer rule in force), the numeric string (`"2"`) and both
/// (`"active:2"`, which must agree). A string is a number when it starts
/// with a digit or `-`. `value_of` maps a name to its value.
///
/// The result may be a value no enumerator has; the derive decides whether
/// the type can hold it.
pub fn read_enum_value<T>(
    r: &mut Reader<'_>,
    signed: bool,
    value_of: impl Fn(&str) -> Option<T>,
) -> Result<T>
where
    T: JsonText + PartialEq + std::str::FromStr,
{
    r.ws();
    if r.peek() != Some(b'"') {
        return T::read_json(r);
    }
    let at = r.pos;
    let text = r.string()?;
    let number = |digits: &str| -> Option<T> {
        if canonical_integer(digits, signed) { digits.parse::<T>().ok() } else { None }
    };
    const BAD_NUMBER: &str = "json: enum number is not a canonical in-range decimal";
    if text.bytes().next().is_some_and(|c| c == b'-' || c.is_ascii_digit()) {
        return number(&text).ok_or_else(|| r.err_at(at, BAD_NUMBER));
    }
    let (name, stated) = match text.split_once(':') {
        Some((name, stated)) => (name, Some(stated)),
        None => (&*text, None),
    };
    let Some(value) = value_of(name) else {
        return Err(r.err_at(at, "json: unknown enumerator name"));
    };
    if let Some(stated) = stated {
        let stated = number(stated).ok_or_else(|| r.err_at(at, BAD_NUMBER))?;
        if stated != value {
            return Err(r.err_at(at, "json: enumerator name and number disagree"));
        }
    }
    Ok(value)
}

/// Opens the IJson form `{"Name":` or the Json form `[index,` of an enum
/// with payloads; returns the integer form to restore.
pub fn write_variant_open(w: &mut Writer, name: &str, index: u32) -> Result<IntForm> {
    let saved = w.member_form(IntForm::Natural, IntForm::Natural);
    if w.ijson {
        w.raw("{");
        w.string(name.as_bytes())?;
        w.raw(":");
    } else {
        w.raw("[");
        w.integer(format_args!("{}", index), false);
        w.raw(",");
    }
    Ok(saved)
}
pub fn write_variant_close(w: &mut Writer, saved: IntForm) {
    w.raw(if w.ijson { "}" } else { "]" });
    w.restore_form(saved);
}

/// Reads the head of an enum with payloads and returns the index of the
/// alternative among `names`.
pub fn read_variant_open(r: &mut Reader<'_>, names: &[&str]) -> Result<(usize, IntForm)> {
    read_variant_open_by(r, names.len(), |key| {
        names.iter().position(|n| *n == key).unwrap_or(names.len())
    })
}

/// The same with the caller's name lookup: `index_of(name)` returns the
/// alternative's index, or `count` for an unknown name. The derive passes a
/// `match` on the names (a switch on length, then compares) instead of a scan.
pub fn read_variant_open_by(
    r: &mut Reader<'_>,
    count: usize,
    index_of: impl Fn(&str) -> usize,
) -> Result<(usize, IntForm)> {
    let saved = r.member_form(IntForm::Natural, IntForm::Natural);
    if r.ijson {
        r.begin(b'{', "ijson: expected a variant object")?;
        r.ws();
        let at = r.pos;
        if r.peek() == Some(b'}') {
            return Err(r.err("ijson: variant object must have one key"));
        }
        // Variant names are checked as I-JSON strings.
        let key = r.string_here(true)?;
        let index = index_of(&key);
        if index >= count {
            return Err(r.err_at(at, "ijson: unknown variant alternative"));
        }
        r.expect(b':', "json: expected ':' after object key")?;
        Ok((index, saved))
    } else {
        r.begin(b'[', "json: expected a [index, value] variant")?;
        let index = u32::read_json(r)? as usize;
        if index >= count {
            return Err(r.err("json: variant index out of range"));
        }
        r.expect(b',', "json: expected ',' in variant")?;
        Ok((index, saved))
    }
}
pub fn read_variant_close(r: &mut Reader<'_>, saved: IntForm) -> Result<()> {
    if r.ijson {
        r.expect(b'}', "ijson: variant object must have one key")?;
    } else {
        r.expect(b']', "json: expected ']' after variant value")?;
    }
    r.end();
    r.restore_form(saved);
    Ok(())
}

// ── The syntax layer behind a cursor ────────────────────────────────────────
//
// JSON text handling is two layers (C++ `psio/json_cursor.hpp`):
//
//   1. Syntax. A cursor owns everything about the text: whitespace,
//      punctuation, string escapes and UTF-8, the RFC 8259 number grammar,
//      nesting depth, skipping a value. It hands out events.
//   2. Types. A walk that checks the cursor's events against the type being
//      read (`psio::pssz::dynamic::json` for the Schema IR): member names,
//      arity, integer ranges and forms, hex digits, variant names, length
//      bounds. It never looks at the text itself.
//
// A cursor is forward-only and single-pass: values are consumed in document
// order, exactly once, and a string is valid only until the next call. That
// is the contract of a streaming parser, so another parser can be put under
// a type walk by implementing `JsonCursor`.

/// The type of the next value, as [`JsonCursor::kind`] reports it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum JsonKind {
    Object,
    Array,
    String,
    Number,
    Bool,
    Null,
}

/// A forward-only JSON event source. Every method consumes input in document
/// order and returns an error, never panics, on malformed text.
pub trait JsonCursor {
    /// The type of the next value; nothing is consumed.
    fn kind(&mut self) -> Result<JsonKind>;
    /// `{`: one nesting level deeper.
    fn begin_object(&mut self) -> Result<()>;
    /// The next member's key (unescaped), or `None` once the object is
    /// closed.
    fn next_field(&mut self) -> Result<Option<Cow<'_, str>>>;
    /// `[`: one nesting level deeper.
    fn begin_array(&mut self) -> Result<()>;
    /// Positions at the next element; `false` once the array is closed.
    fn next_element(&mut self) -> Result<bool>;
    /// A string value, unescaped, and whether the text between the quotes
    /// differed from the value (it was written with escapes).
    fn string(&mut self) -> Result<(Cow<'_, str>, bool)>;
    /// A number as its RFC 8259 token text.
    fn number(&mut self) -> Result<&str>;
    fn boolean(&mut self) -> Result<bool>;
    fn null(&mut self) -> Result<()>;
    /// Skips one value of any shape.
    fn skip(&mut self) -> Result<()>;
    /// Nothing but whitespace remains.
    fn finish(&mut self) -> Result<()>;
    /// A type-layer error at the current position.
    fn error(&self, what: impl Into<Cow<'static, str>>) -> JsonTextError;
}

/// The hand-written [`Reader`] as a cursor. `ijson` applies the I-JSON
/// string rule (no Unicode noncharacters), also to keys and inside skipped
/// values. Nesting deeper than [`MAX_DEPTH`] is an error.
pub struct TextCursor<'a> {
    r: Reader<'a>,
    /// Per open container: whether it has produced an item yet. At most
    /// `MAX_DEPTH` entries.
    fresh: Vec<bool>,
}

impl<'a> TextCursor<'a> {
    pub fn new(text: &'a [u8], ijson: bool) -> Result<Self> {
        let r = Reader::new(text, ijson);
        if text.is_empty() {
            return Err(r.err("json: empty input"));
        }
        Ok(Self { r, fresh: Vec::new() })
    }
    fn open(&mut self, c: u8, expected: &'static str) -> Result<()> {
        self.r.begin(c, expected)?;
        self.fresh.push(true);
        Ok(())
    }
    /// Advances past the separator before the next item, or closes the
    /// container (`false`).
    fn advance(&mut self, close: u8, expected: &'static str) -> Result<bool> {
        let Some(fresh) = self.fresh.last_mut() else {
            return Err(self.r.err("json: cursor is not inside a container"));
        };
        let first = std::mem::replace(fresh, false);
        if self.r.take(close) {
            self.fresh.pop();
            self.r.end();
            return Ok(false);
        }
        if !first && !self.r.take(b',') {
            return Err(self.r.err(expected));
        }
        Ok(true)
    }
}

impl JsonCursor for TextCursor<'_> {
    fn kind(&mut self) -> Result<JsonKind> {
        self.r.kind()
    }
    fn begin_object(&mut self) -> Result<()> {
        self.open(b'{', "json: expected '{'")
    }
    fn next_field(&mut self) -> Result<Option<Cow<'_, str>>> {
        if !self.advance(b'}', "json: expected ',' or '}' in object")? {
            return Ok(None);
        }
        self.r.key().map(Some)
    }
    fn begin_array(&mut self) -> Result<()> {
        self.open(b'[', "json: expected '['")
    }
    fn next_element(&mut self) -> Result<bool> {
        self.advance(b']', "json: expected ',' or ']' in array")
    }
    fn string(&mut self) -> Result<(Cow<'_, str>, bool)> {
        // The reader borrows the input unless the string carried escapes.
        let text = self.r.string()?;
        let escaped = matches!(text, Cow::Owned(_));
        Ok((text, escaped))
    }
    fn number(&mut self) -> Result<&str> {
        self.r.ws();
        let begin = self.r.pos;
        if !matches!(self.r.peek(), Some(b'-' | b'0'..=b'9')) {
            return Err(self.r.err("json: expected a number"));
        }
        self.r.skip_number()?;
        // The number grammar admits ASCII only.
        std::str::from_utf8(&self.r.src[begin..self.r.pos])
            .map_err(|_| self.r.err_at(begin, "json: expected a number"))
    }
    fn boolean(&mut self) -> Result<bool> {
        self.r.ws();
        if self.r.keyword("true") {
            Ok(true)
        } else if self.r.keyword("false") {
            Ok(false)
        } else {
            Err(self.r.err("json: expected true or false"))
        }
    }
    fn null(&mut self) -> Result<()> {
        self.r.ws();
        if self.r.keyword("null") {
            Ok(())
        } else {
            Err(self.r.err("json: malformed keyword"))
        }
    }
    fn skip(&mut self) -> Result<()> {
        self.r.skip_value()
    }
    fn finish(&mut self) -> Result<()> {
        self.r.ws();
        if self.r.pos != self.r.src.len() {
            return Err(self.r.err("json: trailing bytes after value"));
        }
        Ok(())
    }
    fn error(&self, what: impl Into<Cow<'static, str>>) -> JsonTextError {
        self.r.err(what)
    }
}
