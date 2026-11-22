//! The schema-driven reader of the `key` format (docs/key-spec.md): key
//! bytes, a schema and a root type give a dynamic [`Value`], or the reason
//! the bytes are not a key of that type. The writer is [`super::to_key`];
//! the two agree with the typed `psio::key` codec of the type the schema
//! describes.
//!
//! A key is accepted when it is exactly one canonical key of the type: every
//! marker, escape and terminator is the one the writer produces, a float is
//! not `-0`, a `utf8` string is UTF-8, bounded lists and strings are within
//! their bounds, the entries of a map / set are in strictly ascending key
//! order, the bytes of a Nested node are a value of its inner type, and no
//! byte follows the value. The value read is then checked against the schema
//! the way [`super::to_key`] checks the value it writes: member length
//! bounds, caps and, with a registry, the invariants of the custom types.
//!
//! An `Enum` is its underlying integer: every value of the integer is a key,
//! whether or not it is a listed enumerator (the IR does not say whether the
//! enum is closed). A typed closed enum refuses the others.

use super::{
    assoc_id, assoc_shape, bit_shape, bound_bytes, byte_adapter_id, depth, failure, float_size,
    int_size, nested_member, parse_length_bound, unsigned_adapter, AnyType, AnyValue, AssocShape,
    Codec, Member, Options, Pssz, PsszResult, Schema, Value, MAP_ORDER, SET_ORDER,
};
use crate::key::KeyReader;
use crate::pssz::PsszError;
use crate::pssz_checked as checked;

const NONE: &str = "pssz dynamic: this type has no key encoding";
const BOXED: &str = "pssz dynamic: a boxed type has no key encoding";
const NOT_COMPOSITE: &str = "pssz dynamic: a key prefix needs a tuple or record type";
const TOO_MANY: &str = "pssz dynamic: a key prefix has more elements than the type";
const LIST_BOUND: &str = "key: list exceeds its bound";

fn key_error(error: crate::key::KeyError) -> PsszError {
    PsszError(error.0)
}

/// One read of a key: the cursor, and the byte count of the Nested node
/// read last (what a member `lengthBound` on a Nested member counts).
struct KeyDecoder<'c, 'a, 'k> {
    codec: &'c Codec<'a>,
    r: KeyReader<'k>,
    key: &'k [u8],
    nested_len: Option<usize>,
}

impl<'c, 'a, 'k> KeyDecoder<'c, 'a, 'k> {
    fn new(codec: &'c Codec<'a>, key: &'k [u8]) -> Self {
        Self { codec, r: KeyReader::new(key), key, nested_len: None }
    }
    fn pos(&self) -> usize {
        self.key.len() - self.r.remaining().len()
    }
    fn take(&mut self, n: usize) -> PsszResult<&'k [u8]> {
        self.r.take(n).map_err(key_error)
    }
    fn byte(&mut self) -> PsszResult<u8> {
        self.r.byte().map_err(key_error)
    }
    fn escaped(&mut self) -> PsszResult<Vec<u8>> {
        self.r.escaped().map_err(key_error)
    }

    fn read(&mut self, input: &AnyType, n: usize) -> PsszResult<Value> {
        depth(n)?;
        let schema = self.codec.schema;
        let (ty, boxed) = schema.wire_boxed(input)?;
        if boxed {
            return Err(failure(BOXED));
        }
        Ok(match &ty.value {
            AnyValue::Int(i) if i.bits == 1 => return Err(failure(NONE)),
            AnyValue::Int(i) => {
                let size = int_size(i)?;
                let bytes = self.take(size)?;
                if i.bits == 256 {
                    return Ok(Value::Bytes(bytes.iter().rev().copied().collect()));
                }
                let mut raw = [0u8; 16];
                raw[16 - size..].copy_from_slice(bytes);
                let bits = u128::from_be_bytes(raw);
                if i.isSigned {
                    // The sign bit is flipped in a key; sign-extend.
                    let flipped = bits ^ (1u128 << (i.bits - 1));
                    let shift = 128 - i.bits;
                    Value::Signed(((flipped << shift) as i128) >> shift)
                } else {
                    Value::Unsigned(bits)
                }
            }
            AnyValue::Float(f) => {
                const ZERO: PsszError = PsszError("key: negative zero is not canonical");
                if float_size(f)? == 4 {
                    const SIGN: u32 = 1 << 31;
                    let mut bits = u32::from_be_bytes(self.take(4)?.try_into().unwrap());
                    bits = if bits & SIGN != 0 { bits ^ SIGN } else { !bits };
                    if bits == SIGN {
                        return Err(ZERO);
                    }
                    Value::Float32(f32::from_bits(bits))
                } else {
                    const SIGN: u64 = 1 << 63;
                    let mut bits = u64::from_be_bytes(self.take(8)?.try_into().unwrap());
                    bits = if bits & SIGN != 0 { bits ^ SIGN } else { !bits };
                    if bits == SIGN {
                        return Err(ZERO);
                    }
                    Value::Float64(f64::from_bits(bits))
                }
            }
            AnyValue::Custom(c) if c.id == "bool" => {
                unsigned_adapter(schema, &c.r#type, 1)?;
                match self.byte()? {
                    0 => Value::Bool(false),
                    1 => Value::Bool(true),
                    _ => return Err(failure("key: invalid bool")),
                }
            }
            AnyValue::Custom(c) if c.id == "char" => {
                unsigned_adapter(schema, &c.r#type, 8)?;
                Value::Unsigned(u128::from(self.byte()?))
            }
            // A string / byte list: escaped and terminated. A fixed byte
            // array: its bytes.
            AnyValue::Custom(c) if byte_adapter_id(&c.id) => {
                let bytes = match &schema.wire(&c.r#type)?.value {
                    AnyValue::Array(a) => {
                        let len = usize::try_from(a.len)
                            .map_err(|_| failure("pssz dynamic: bound overflow"))?;
                        self.take(len)?.to_vec()
                    }
                    _ => self.escaped()?,
                };
                bound_bytes(schema, &c.r#type, bytes.len())?;
                match c.id.as_str() {
                    "utf8" => Value::String(
                        String::from_utf8(bytes).map_err(|_| failure("key: invalid UTF-8"))?,
                    ),
                    "string" => match String::from_utf8(bytes) {
                        Ok(text) => Value::String(text),
                        Err(error) => Value::Bytes(error.into_bytes()),
                    },
                    _ => Value::Bytes(bytes),
                }
            }
            AnyValue::Custom(c) if c.id == "bitvector" => {
                let (count, _) = bit_shape(schema, c)?;
                let bytes = self.take(count.div_ceil(8))?;
                if count % 8 != 0 && bytes[bytes.len() - 1] >> (count % 8) != 0 {
                    return Err(failure("key: bitvector padding bits are set"));
                }
                Value::Bits { bytes: bytes.to_vec(), bit_count: count as u64 }
            }
            AnyValue::Custom(c) if assoc_id(&c.id) => {
                let shape = assoc_shape(schema, c)?;
                Value::Sequence(self.read_assoc(&shape, n)?)
            }
            AnyValue::Custom(_) | AnyValue::Resource(_) => return Err(failure(NONE)),
            AnyValue::Option(o) => match self.byte()? {
                0 => Value::Optional(None),
                1 => Value::Optional(Some(Box::new(self.read(&o.r#type, n + 1)?))),
                _ => return Err(failure("key: invalid optional marker")),
            },
            AnyValue::List(l) => Value::Sequence(self.read_list(&l.r#type, None, n)?),
            AnyValue::BoundedList(l) => {
                Value::Sequence(self.read_list(&l.r#type, Some(l.maxCount), n)?)
            }
            // A fixed array is its elements with no framing.
            AnyValue::Array(a) => {
                let mut items = Vec::new();
                for i in 0..a.len {
                    let before = self.pos();
                    items.push(self.read(&a.r#type, n + 1)?);
                    // Elements with an empty key: the count is all there is.
                    if i == 0 && self.pos() == before && a.len > 1 << 16 {
                        return Err(failure("pssz dynamic: array count overflow"));
                    }
                }
                Value::Sequence(items)
            }
            AnyValue::Tuple(t) => Value::Sequence(
                t.members
                    .iter()
                    .map(|member| self.read(member, n + 1))
                    .collect::<PsszResult<_>>()?,
            ),
            AnyValue::Struct(s) => Value::Record(self.read_members(&s.members, n)?),
            AnyValue::Object(o) => Value::Record(self.read_members(&o.members, n)?),
            AnyValue::Variant(v) => {
                let selector = self.byte()? as usize;
                if v.members.len() > 256 || selector >= v.members.len() {
                    return Err(failure("key: variant index out of range"));
                }
                Value::Variant {
                    selector,
                    value: Box::new(self.read(&v.members[selector].r#type, n + 1)?),
                }
            }
            // The key of the byte list holding the inner encoding. The bytes
            // are a value of the inner type (unknown appended optional
            // fields are accepted, as in a PSSZ read) unless they are opaque.
            AnyValue::Nested(nested) => {
                let bytes = self.escaped()?;
                self.nested_len = Some(bytes.len());
                if self.codec.opaque {
                    if !bytes.is_empty() {
                        depth(n + 1)?;
                    }
                    Value::Bytes(bytes)
                } else {
                    self.codec.nested_decode(&nested.r#type, &bytes, n + 1)?
                }
            }
            _ => return Err(failure(NONE)),
        })
    }

    /// The elements are octets (u8, i8 or char, reached through aliases
    /// only): a list of them has the string framing.
    fn octets(&self, element: &AnyType) -> PsszResult<bool> {
        Ok(match &self.codec.schema.resolve(element)?.value {
            AnyValue::Int(i) => i.bits == 8,
            AnyValue::Custom(c) => c.id == "char",
            _ => false,
        })
    }
    /// The elements of an octet list, and each element's key byte.
    fn read_octets(
        &mut self,
        element: &AnyType,
        max: Option<u64>,
        n: usize,
    ) -> PsszResult<(Vec<Value>, Vec<u8>)> {
        let bytes = self.escaped()?;
        if max.is_some_and(|max| bytes.len() as u128 > max as u128) {
            return Err(failure(LIST_BOUND));
        }
        let items = bytes
            .iter()
            .map(|byte| {
                KeyDecoder::new(self.codec, std::slice::from_ref(byte)).read(element, n + 1)
            })
            .collect::<PsszResult<_>>()?;
        Ok((items, bytes))
    }
    /// `01` before each element, `00` after the last. `Ok(false)`: the end.
    fn list_marker(&mut self, count: usize, max: Option<u64>) -> PsszResult<bool> {
        match self.byte()? {
            0 => return Ok(false),
            1 => (),
            _ => return Err(failure("key: invalid list element marker")),
        }
        if max.is_some_and(|max| count as u128 >= max as u128) {
            return Err(failure(LIST_BOUND));
        }
        Ok(true)
    }
    fn read_list(&mut self, element: &AnyType, max: Option<u64>, n: usize) -> PsszResult<Vec<Value>> {
        if self.octets(element)? {
            return Ok(self.read_octets(element, max, n)?.0);
        }
        let mut items = Vec::new();
        while self.list_marker(items.len(), max)? {
            items.push(self.read(element, n + 1)?);
        }
        Ok(items)
    }
    /// A map / set: the list of its entries, in strictly ascending order of
    /// the key bytes of each entry's key.
    fn read_assoc(&mut self, shape: &AssocShape<'_>, n: usize) -> PsszResult<Vec<Value>> {
        let order = failure(if shape.map { MAP_ORDER } else { SET_ORDER });
        if self.octets(shape.element)? {
            let (items, bytes) = self.read_octets(shape.element, shape.max, n)?;
            if bytes.windows(2).any(|pair| pair[0] >= pair[1]) {
                return Err(order);
            }
            return Ok(items);
        }
        let key = self.key;
        let mut items = Vec::new();
        let mut last: Option<&[u8]> = None;
        while self.list_marker(items.len(), shape.max)? {
            let begin = self.pos();
            let entry_key = self.read(shape.key, n + 1)?;
            let this = &key[begin..self.pos()];
            if last.is_some_and(|last| last >= this) {
                return Err(order);
            }
            last = Some(this);
            items.push(match shape.value {
                Some(value) => Value::Sequence(vec![entry_key, self.read(value, n + 1)?]),
                None => entry_key,
            });
        }
        Ok(items)
    }
    /// The members in schema order. A `lengthBound` on a Nested member
    /// counts the bytes read for it; the other bounds are checked with the
    /// value.
    fn read_members(&mut self, members: &[Member], n: usize) -> PsszResult<Vec<(String, Value)>> {
        members
            .iter()
            .map(|member| {
                self.nested_len = None;
                let value = self.read(&member.r#type, n + 1)?;
                if let (Some(len), Some(bound)) =
                    (self.nested_len, parse_length_bound(&member.attributes)?)
                {
                    if nested_member(self.codec.schema, &member.r#type)? && !bound.admits(len as u64)
                    {
                        return Err(failure("pssz dynamic: member length bound violated"));
                    }
                }
                Ok((member.name.clone(), value))
            })
            .collect()
    }
}

impl<'a> Codec<'a> {
    fn from_key(&self, ty: &AnyType, key: &[u8]) -> PsszResult<Value> {
        self.check(ty)?;
        // The bytes of a Nested node are read as a PSSZ reader reads them.
        let _session = checked::DecodeSession::enter(true);
        let mut decoder = KeyDecoder::new(self, key);
        let value = decoder.read(ty, 0)?;
        if !decoder.r.remaining().is_empty() {
            return Err(failure("key: trailing bytes"));
        }
        // The value against the schema, and the invariants.
        self.encode_at(ty, &value, 0)?;
        Ok(value)
    }
}

/// The value whose key is `key`, a key of `ty`: the reverse of
/// [`super::to_key`]. `key` must be exactly one canonical key of the type.
///
/// A Nested node gives its inner value (its bytes are validated as the inner
/// type); with `NestedBytes::Opaque` it gives `Value::Bytes`, not examined.
/// A map / set gives its entries in key order.
pub fn from_key(schema: &Schema, ty: &AnyType, key: &[u8]) -> PsszResult<Value> {
    from_key_with(schema, ty, key, Options::default())
}
/// [`from_key`] with a handler registry (the invariants of the custom types
/// are checked, and a strict registry requires every handler) and / or
/// opaque nested bytes.
pub fn from_key_with(
    schema: &Schema,
    ty: &AnyType,
    key: &[u8],
    options: Options<'_>,
) -> PsszResult<Value> {
    Codec::with::<Pssz>(schema, options)?.from_key(ty, key)
}
/// Checks that `key` is exactly one canonical key of `ty`: what
/// [`from_key`] accepts.
pub fn validate_key(schema: &Schema, ty: &AnyType, key: &[u8]) -> PsszResult<()> {
    validate_key_with(schema, ty, key, Options::default())
}
/// [`validate_key`] with a handler registry and / or opaque nested bytes.
pub fn validate_key_with(
    schema: &Schema,
    ty: &AnyType,
    key: &[u8],
    options: Options<'_>,
) -> PsszResult<()> {
    from_key_with(schema, ty, key, options).map(drop)
}

/// The key of the first `elements.len()` elements of the composite type
/// `ty` (a tuple or a record; its members in schema order): the prefix every
/// full key whose leading elements are `elements` starts with. Its exclusive
/// upper bound is `psio::key::key_prefix_upper_bound`, which needs no type.
///
/// Each element is checked as the value it is; the composite as a whole is
/// not (a prefix is not a value of it).
pub fn key_prefix(schema: &Schema, ty: &AnyType, elements: &[Value]) -> PsszResult<Vec<u8>> {
    key_prefix_with(schema, ty, elements, Options::default())
}
/// [`key_prefix`] with a handler registry and / or opaque nested bytes.
pub fn key_prefix_with(
    schema: &Schema,
    ty: &AnyType,
    elements: &[Value],
    options: Options<'_>,
) -> PsszResult<Vec<u8>> {
    let codec = Codec::with::<Pssz>(schema, options)?;
    codec.check(ty)?;
    let (wire, boxed) = schema.wire_boxed(ty)?;
    if boxed {
        return Err(failure(BOXED));
    }
    let members: Vec<(&AnyType, Option<&Member>)> = match &wire.value {
        AnyValue::Tuple(t) => t.members.iter().map(|member| (&**member, None)).collect(),
        AnyValue::Struct(s) => s.members.iter().map(|m| (&*m.r#type, Some(m))).collect(),
        AnyValue::Object(o) => o.members.iter().map(|m| (&*m.r#type, Some(m))).collect(),
        _ => return Err(failure(NOT_COMPOSITE)),
    };
    if elements.len() > members.len() {
        return Err(failure(TOO_MANY));
    }
    let mut out = Vec::new();
    for ((element, member), value) in members.into_iter().zip(elements) {
        let (_, bytes) = codec.encoded_field(element, value, 0)?;
        if let Some(bound) = member.map(|m| parse_length_bound(&m.attributes)).transpose()?.flatten()
        {
            if nested_member(schema, element)? {
                codec.nested_bound(element, &bytes, &bound)?;
            } else if !bound.admits_value(value) {
                return Err(failure("pssz dynamic: member length bound violated"));
            }
        }
        codec.key_at(element, value, &mut out, 0)?;
    }
    Ok(out)
}
