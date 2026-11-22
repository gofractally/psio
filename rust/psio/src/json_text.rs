// The JSON text formats `Json` (RFC 8259) and `IJson` (RFC 7493), the Rust
// counterpart of C++ `psio::json` / `psio::ijson` (cpp/include/psio/json.hpp).
// Both languages produce the same text for the same value and accept and
// reject the same input; conformance/json-vectors.json is the shared proof.
//
//   Json    every integer is a bare number, a byte sequence is an array of
//           numbers, an enum with payloads is `[index, value]`.
//   IJson   a 64-bit integer is a bare number while its magnitude is at most
//           2^53 - 1 and a decimal string beyond that (either form is read,
//           a bare number only within that range), a byte sequence (`Vec<u8>`,
//           `[u8; N]`) is one UPPERCASE hex string, an enum with payloads is
//           `{"Name": value}`.
//
// Everything else is common: records are objects in declaration order,
// `Vec` / arrays / tuples are arrays (`()` is `[]`), `Option` is null or the
// value, `String` and `RawString` are JSON strings, a field-less
// `#[repr(int)]` enum is its integer.
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
    /// Static description.
    pub what: &'static str,
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
#[macro_export]
macro_rules! json_text_form {
    ($ty:ty) => {
        impl $crate::json::JsonText for $ty {
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

/// The text of `value` as UTF-8 bytes.
pub fn to_vec<F: JsonFormat, T: JsonText>(value: &T) -> Result<Vec<u8>> {
    let mut w = Writer::new(F::IJSON);
    value.write_json(&mut w)?;
    Ok(w.out)
}

/// The text of `value`.
pub fn to_string<F: JsonFormat, T: JsonText>(value: &T) -> Result<String> {
    // The writer only emits validated UTF-8.
    Ok(String::from_utf8(to_vec::<F, T>(value)?).expect("the JSON writer emits UTF-8"))
}

/// Reads `text` as exactly one `T`.
pub fn from_slice<F: JsonFormat, T: JsonText>(text: &[u8]) -> Result<T> {
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

/// Reads `text` as exactly one `T`.
pub fn from_str<F: JsonFormat, T: JsonText>(text: &str) -> Result<T> {
    from_slice::<F, T>(text.as_bytes())
}

/// Checks that `text` is exactly one `T`. Accepts what [`from_slice`]
/// accepts.
pub fn validate<F: JsonFormat, T: JsonText>(text: &[u8]) -> Result<()> {
    from_slice::<F, T>(text).map(|_| ())
}

macro_rules! format_impls {
    ($tag:ty) => {
        impl<T: JsonText> Encode<$tag> for T {
            fn encode_into(&self, out: &mut Vec<u8>) -> Result<usize> {
                let text = to_vec::<$tag, T>(self)?;
                out.extend_from_slice(&text);
                Ok(text.len())
            }
        }
        impl<T: JsonText> Decode<$tag> for T {
            fn decode(bytes: &[u8]) -> Result<Self> {
                from_slice::<$tag, T>(bytes)
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

// ── Writer ──────────────────────────────────────────────────────────────────

/// Output sink of [`JsonText::write_json`].
pub struct Writer {
    out: Vec<u8>,
    ijson: bool,
    form: IntForm,
}

const HEX_UPPER: &[u8; 16] = b"0123456789ABCDEF";
const HEX_LOWER: &[u8; 16] = b"0123456789abcdef";

fn is_noncharacter(cp: u32) -> bool {
    (0xFDD0..=0xFDEF).contains(&cp) || (cp & 0xFFFE) == 0xFFFE
}

impl Writer {
    /// An empty sink for `IJson` (`ijson`) or `Json` text.
    pub fn new(ijson: bool) -> Self {
        Self { out: Vec::new(), ijson, form: IntForm::Natural }
    }
    /// The text written so far (UTF-8).
    pub fn into_bytes(self) -> Vec<u8> {
        self.out
    }
    pub fn is_ijson(&self) -> bool {
        self.ijson
    }
    fn format(&self) -> &'static str {
        if self.ijson { "ijson" } else { "json" }
    }
    pub fn err(&self, what: &'static str) -> JsonTextError {
        JsonTextError { what, offset: self.out.len(), format: self.format() }
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

    /// `wide`: a 64-bit integer whose magnitude exceeds 2^53 - 1.
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
        for &b in bytes {
            self.out.push(HEX_UPPER[(b >> 4) as usize]);
            self.out.push(HEX_UPPER[(b & 15) as usize]);
        }
        self.out.push(b'"');
    }

    /// Shortest round-trip decimal in the layout of C++ `std::to_chars`:
    /// fixed or scientific, whichever is shorter (fixed on a tie), with a
    /// signed exponent of at least two digits.
    fn float(
        &mut self,
        shortest: String,
        precise: impl Fn(usize) -> String,
        exact_integer: impl FnOnce() -> String,
    ) {
        // `shortest` is Rust's `{:e}`: [-]d[.ddd]e[-]x
        fn split(text: &str) -> (&str, String, i32) {
            let (mantissa, exponent) = text.split_once('e').expect("{:e} has an exponent");
            let exponent: i32 = exponent.parse().expect("{:e} exponent");
            let (sign, mantissa) = match mantissa.strip_prefix('-') {
                Some(rest) => ("-", rest),
                None => ("", mantissa),
            };
            (sign, mantissa.chars().filter(|c| *c != '.').collect(), exponent)
        }
        let (sign, mut digits, exponent) = split(&shortest);
        // When the value lies exactly halfway between two shortest
        // candidates, std::to_chars (Ryu) keeps the even last digit and
        // Rust rounds up. That needs one more exact digit, a 5: probe for it
        // cheaply, then confirm against the exact expansion.
        let probe_text = precise(digits.len());
        let probe = split(&probe_text);
        if probe.2 == exponent && probe.1.len() == digits.len() + 1 && probe.1.ends_with('5') {
            let exact_text = precise(1100);
            let exact = split(&exact_text);
            let trimmed = exact.1.trim_end_matches('0');
            if exact.2 == exponent && trimmed == probe.1 {
                let down = &trimmed[..digits.len()];
                if (down.as_bytes()[down.len() - 1] - b'0') % 2 == 0 {
                    digits = down.to_owned();
                }
            }
        }
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

fn hex_digit(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

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
    pub fn err(&self, what: &'static str) -> JsonTextError {
        self.err_at(self.pos, what)
    }
    pub fn err_at(&self, offset: usize, what: &'static str) -> JsonTextError {
        JsonTextError { what, offset, format: if self.ijson { "ijson" } else { "json" } }
    }
    fn peek(&self) -> Option<u8> {
        self.src.get(self.pos).copied()
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

    /// The characters of an integer at the cursor: either the digits between
    /// quotes or a bare number token, per the form in force.
    fn integer_token(&mut self, wide: bool) -> Result<(&'a str, bool)> {
        self.ws();
        let as_string = match self.form {
            IntForm::String => true,
            IntForm::Number => false,
            IntForm::Natural => self.ijson && wide,
        };
        let src = self.src;
        match self.peek() {
            None => Err(self.err("json: unexpected end of input")),
            Some(b'"') => {
                if !as_string {
                    return Err(self.err("json: expected a number, found a string"));
                }
                let begin = self.pos + 1;
                let mut q = begin;
                while q < src.len() && src[q] != b'"' {
                    q += 1;
                }
                if q >= src.len() {
                    return Err(self.err("json: unterminated string"));
                }
                let text = std::str::from_utf8(&src[begin..q]).map_err(|_| {
                    self.err("json: integer string is not a canonical in-range decimal")
                })?;
                self.pos = q + 1;
                Ok((text, false))
            }
            Some(_) => {
                if as_string && self.form == IntForm::String {
                    return Err(self.err("json: expected an integer string"));
                }
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
                Ok((std::str::from_utf8(&src[begin..q]).expect("ASCII"), as_string))
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

fn read_hex(r: &mut Reader<'_>, exact: Option<usize>) -> Result<Vec<u8>> {
    r.ws();
    if r.peek() != Some(b'"') {
        return Err(r.err("ijson: expected a hex string"));
    }
    let src = r.src;
    let begin = r.pos + 1;
    let mut q = begin;
    while q < src.len() && hex_digit(src[q]).is_some() {
        q += 1;
    }
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
    Ok(digits
        .chunks_exact(2)
        .map(|pair| (hex_digit(pair[0]).unwrap() << 4) | hex_digit(pair[1]).unwrap())
        .collect())
}

macro_rules! float {
    ($($ty:ty)*) => {$(
        impl JsonText for $ty {
            fn write_json(&self, w: &mut Writer) -> Result<()> {
                if !self.is_finite() {
                    // C++ psio::json prints "nan" / "inf" here, which is not
                    // JSON; no reader accepts it. Both Rust formats refuse.
                    return Err(w.err("json: non-finite float has no JSON text"));
                }
                let value = *self;
                w.float(
                    format!("{:e}", value),
                    |precision| format!("{:.*e}", precision, value),
                    || format!("{}", value.abs() as f64 as u128),
                );
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
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        if self.0.len() > N {
            return Err(w.err("json: string exceeds its bound"));
        }
        w.string(self.0.as_bytes())
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        r.ws();
        let at = r.pos;
        let text = r.string()?;
        if text.len() > N {
            return Err(r.err_at(at, "json: string exceeds its bound"));
        }
        Ok(Self(text.into_owned()))
    }
}

impl<const N: usize> JsonText for crate::pssz::BoundedRawString<N> {
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        if self.0.len() > N {
            return Err(w.err("json: string exceeds its bound"));
        }
        w.string(&self.0)
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        r.ws();
        let at = r.pos;
        let text = r.string()?;
        if text.len() > N {
            return Err(r.err_at(at, "json: string exceeds its bound"));
        }
        Ok(Self(text.into_owned().into_bytes()))
    }
}

impl<T: JsonText, const N: usize> JsonText for crate::pssz::BoundedList<T, N> {
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        if self.0.len() > N {
            return Err(w.err("json: array exceeds its bound"));
        }
        T::write_json_slice(&self.0, w)
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        r.ws();
        let at = r.pos;
        let items = T::read_json_vec(r)?;
        if items.len() > N {
            return Err(r.err_at(at, "json: array exceeds its bound"));
        }
        Ok(Self(items))
    }
}

impl<T: JsonText> JsonText for Vec<T> {
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        T::write_json_slice(self, w)
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        T::read_json_vec(r)
    }
}

impl<T: JsonText, const N: usize> JsonText for [T; N] {
    fn write_json(&self, w: &mut Writer) -> Result<()> {
        T::write_json_slice(self, w)
    }
    fn read_json(r: &mut Reader<'_>) -> Result<Self> {
        T::read_json_array::<N>(r)
    }
}

impl<T: JsonText> JsonText for Option<T> {
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

// ── Support for #[derive(JsonText)] on enums ────────────────────────────────

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
    fn error(&self, what: &'static str) -> JsonTextError;
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
        self.r.ws();
        match self.r.peek() {
            None => Err(self.r.err("json: unexpected end of input")),
            Some(b'{') => Ok(JsonKind::Object),
            Some(b'[') => Ok(JsonKind::Array),
            Some(b'"') => Ok(JsonKind::String),
            Some(b't' | b'f') => Ok(JsonKind::Bool),
            Some(b'n') => Ok(JsonKind::Null),
            Some(b'-' | b'0'..=b'9') => Ok(JsonKind::Number),
            Some(_) => Err(self.r.err("json: unexpected character at value start")),
        }
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
    fn error(&self, what: &'static str) -> JsonTextError {
        self.r.err(what)
    }
}
