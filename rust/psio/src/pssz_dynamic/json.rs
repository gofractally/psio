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
//! * string adapter: JSON string (UTF-8; IJson: no noncharacters)
//! * hex adapter: IJson one hex string (UPPERCASE on output); Json an array
//!   of numbers
//! * Int: number; a 64-bit integer under IJson is a bare number while
//!   |v| <= 2^53 - 1, a decimal string beyond (either form is read, bare
//!   only in that range); a member's `jsonInt` / `ijsonInt` attribute forces
//!   one form and reaches the integer through option, list and array
//! * bool, Float: true / false; shortest round-trip decimal
//!
//! No JSON form (an error, not a guess): integers wider than 64 bits and the
//! bitvector / bitlist adapters. A non-finite float has no JSON text.
//!
//! The reader is a type walk over a [`JsonCursor`]: it holds no text
//! positions and does no tokenizing, so another parser can be put under it.
//! Everything is checked: errors, never panics; nesting and schema recursion
//! are limited by `MAX_DEPTH`.
use super::{metadata, parse_length_bound, Value};
use crate::json::{
    write_variant_close, write_variant_open, IntForm, JsonCursor, JsonFormat, JsonKind,
    JsonText, JsonTextError, TextCursor, Writer, MAX_DEPTH,
};
use crate::pssz::{Pssz, PsszError};
use crate::pssz_schema_ir::{AnyType, AnyValue, Attribute, Custom, Int, Member, Schema, Variant};

/// Why a schema-driven conversion failed: the schema or the PSSZ bytes
/// ([`PsszError`]), or the text ([`JsonTextError`]).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SchemaJsonError {
    Pssz(PsszError),
    Json(JsonTextError),
}
impl SchemaJsonError {
    /// The static description, without position.
    pub fn what(&self) -> &'static str {
        match self {
            Self::Pssz(e) => e.0,
            Self::Json(e) => e.what,
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

// ── Reading: the type walk over a cursor ────────────────────────────────────

struct Reader<'a, C: JsonCursor> {
    schema: &'a Schema,
    c: &'a mut C,
    ijson: bool,
}

impl<C: JsonCursor> Reader<'_, C> {
    fn fail<T>(&self, what: &'static str) -> Text<T> {
        Err(self.c.error(what))
    }
    fn resolve<'s>(&self, schema: &'s Schema, ty: &'s AnyType) -> Text<&'s AnyType> {
        schema.resolve(ty).map_err(|_| self.c.error("pssz dynamic: unresolved named type"))
    }

    fn integer(&mut self, ty: &Int, form: IntForm) -> Text<Value> {
        if !matches!(ty.bits, 1 | 8 | 16 | 32 | 64) {
            return self.fail("json: integers wider than 64 bits have no JSON form");
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

    fn custom(&mut self, ty: &Custom, form: IntForm) -> Text<Value> {
        match ty.id.as_str() {
            "bool" => return Ok(Value::Bool(self.c.boolean()?)),
            "char" => return self.integer(&BYTE, form),
            "bitvector" | "bitlist" => return self.fail("json: bit types have no JSON form"),
            "string" | "hex" => (),
            _ => return self.fail("pssz dynamic: unsupported custom adapter"),
        }
        let Some(shape) = ByteShape::of(self.schema, ty) else {
            return self.fail("pssz dynamic: byte adapter requires byte container");
        };
        if ty.id == "string" {
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
            if bound.is_some_and(|bound| !bound.admits_value(&value)) {
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
            AnyValue::Custom(c) => self.custom(c, form),
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
}

impl ValueWriter<'_> {
    fn fail<T>(&self, what: &'static str) -> Text<T> {
        Err(self.w.err(what))
    }
    fn resolve<'s>(&self, schema: &'s Schema, ty: &'s AnyType) -> Text<&'s AnyType> {
        schema.resolve(ty).map_err(|_| self.w.err("pssz dynamic: unresolved named type"))
    }

    /// The form in force is the writer's (set per record member).
    fn integer(&mut self, ty: &Int, value: &Value) -> Text<()> {
        if ty.bits > 64 {
            return self.fail("json: integers wider than 64 bits have no JSON form");
        }
        const MISMATCH: &str = "pssz dynamic: integer value does not match the schema";
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

    fn custom(&mut self, ty: &Custom, value: &Value) -> Text<()> {
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
            ("string" | "hex", _) => (),
            _ => return self.fail("pssz dynamic: unsupported custom adapter"),
        }
        let Some(shape) = ByteShape::of(self.schema, ty) else {
            return self.fail("pssz dynamic: byte adapter requires byte container");
        };
        if ty.id == "string" {
            let Value::String(text) = value else {
                return self.fail("pssz dynamic: string value required");
            };
            if !shape.admits(text.len()) {
                return self.fail("json: string exceeds its bound");
            }
            return self.w.string(text.as_bytes());
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
            if bound.is_some_and(|bound| !bound.admits_value(field)) {
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
            AnyValue::Float(f) => match value {
                Value::Float32(v) if f.exp == 8 => v.write_json(&mut self.w),
                Value::Float64(v) if f.exp == 11 => v.write_json(&mut self.w),
                _ => self.fail("pssz dynamic: float value does not match the schema"),
            },
            AnyValue::Custom(c) => self.custom(c, value),
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
            _ => self.fail("pssz dynamic: unsupported schema type"),
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
    let value = Reader { schema, c: &mut *cursor, ijson }.read(root, IntForm::Natural, 0)?;
    cursor.finish()?;
    Ok(value)
}

// ── Text <-> Value ──────────────────────────────────────────────────────────

/// `text` (exactly one `root` value) as a [`Value`].
pub fn json_to_value<F: JsonFormat>(
    schema: &Schema,
    root: &AnyType,
    text: &[u8],
) -> Result<Value, SchemaJsonError> {
    // Schema errors (unsupported shapes, bad attributes) come first.
    metadata(schema, root, true)?;
    let mut cursor = TextCursor::new(text, F::IJSON)?;
    Ok(read_json(schema, root, F::IJSON, &mut cursor)?)
}

/// The text of `value`, a value of `root`.
pub fn value_to_json<F: JsonFormat>(
    schema: &Schema,
    root: &AnyType,
    value: &Value,
) -> Result<String, SchemaJsonError> {
    metadata(schema, root, true)?;
    let mut writer = ValueWriter { schema, w: Writer::new(F::IJSON) };
    writer.write(root, value, 0)?;
    // The writer only emits validated UTF-8.
    String::from_utf8(writer.w.into_bytes()).map_err(|_| {
        JsonTextError { what: "json: string is not valid UTF-8", offset: 0, format: F::NAME }.into()
    })
}

// ── What a schema-only consumer calls ───────────────────────────────────────

/// JSON / I-JSON text of a `root` value -> its canonical PSSZ bytes (the
/// automatic profile: the bytes of the typed `Pssz` codec).
pub fn json_to_pssz<F: JsonFormat>(
    schema: &Schema,
    root: &AnyType,
    text: &[u8],
) -> Result<Vec<u8>, SchemaJsonError> {
    let value = json_to_value::<F>(schema, root, text)?;
    Ok(super::encode::<Pssz>(schema, root, &value)?)
}

/// PSSZ bytes of a `root` value -> its JSON / I-JSON text. The bytes are
/// fully validated. Unknown appended optional fields are accepted, dropped
/// from the text and reported in the second result (a caller that must not
/// lose them rejects when it is set).
pub fn pssz_to_json<F: JsonFormat>(
    schema: &Schema,
    root: &AnyType,
    bytes: &[u8],
) -> Result<(String, bool), SchemaJsonError> {
    let decoded = super::decode_compatible::<Pssz>(schema, root, bytes)?;
    let text = value_to_json::<F>(schema, root, &decoded.value)?;
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
