//! Checked runtime codec consuming the actual C++ schema IR mirror.
//! Unsupported IR markers/adapters return errors; no legacy schema fallback.
use crate::pssz::{Decoded, PsszError, PsszFormat, PsszResult};
use crate::pssz_checked::{self as checked, Layout};
use crate::pssz_schema_ir::{AnyType, AnyValue, Attribute, Member, Schema};
use serde::{Deserialize, Serialize};
use std::collections::HashSet;

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
    PsszError(message)
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
fn attributes(values: &[Attribute], layout: bool) -> PsszResult<()> {
    for value in values {
        match value.name.as_str() {
            "doc" | "description" | "visible" => (),
            "definitionWillNotChange" if layout => (),
            _ => {
                return Err(failure(
                    "pssz dynamic: unsupported schema constraint or annotation",
                ))
            }
        }
    }
    Ok(())
}
fn metadata(schema: &Schema, root: &AnyType) -> PsszResult<()> {
    let mut pending = vec![root];
    let mut seen = HashSet::new();
    while let Some(ty) = pending.pop() {
        let ty = schema.resolve(ty)?;
        if !seen.insert(ty as *const AnyType as usize) {
            continue;
        }
        let fields = match &ty.value {
            AnyValue::Object(o) => {
                attributes(&o.attributes, true)?;
                Some(o.members.as_slice())
            }
            AnyValue::Struct(s) => {
                attributes(&s.attributes, true)?;
                Some(s.members.as_slice())
            }
            AnyValue::Variant(v) => {
                attributes(&v.attributes, false)?;
                if v.members.is_empty() || v.members.len() > 256 {
                    return Err(failure("pssz dynamic: invalid variant schema"));
                }
                Some(v.members.as_slice())
            }
            AnyValue::Tuple(t) => {
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
                    "string" | "hex" => {
                        byte_container(schema, &c.r#type)?;
                    }
                    "bitvector" | "bitlist" => {
                        bit_shape(schema, c)?;
                    }
                    _ => return Err(failure("pssz dynamic: unsupported custom adapter")),
                }
                pending.push(&c.r#type);
                None
            }
            AnyValue::Int(i) => {
                int_size(i.bits)?;
                None
            }
            AnyValue::Float(f) if matches!((f.exp, f.mantissa), (8, 23 | 24) | (11, 52 | 53)) => {
                None
            }
            _ => return Err(failure("pssz dynamic: unsupported schema type")),
        };
        if let Some(fields) = fields {
            let mut names = HashSet::new();
            for field in fields {
                if !names.insert(field.name.as_str()) {
                    return Err(failure("pssz dynamic: duplicate schema member"));
                }
                attributes(&field.attributes, false)?;
                pending.push(&field.r#type);
            }
        }
    }
    Ok(())
}
fn int_size(bits: u32) -> PsszResult<usize> {
    match bits {
        1 => Ok(1),
        8 | 16 | 32 | 64 | 128 => Ok(bits as usize / 8),
        _ => Err(failure("pssz dynamic: unsupported integer width")),
    }
}
fn unsigned_adapter(schema: &Schema, ty: &AnyType, bits: u32) -> PsszResult<()> {
    if matches!(schema.resolve(ty)?.value, AnyValue::Int(ref i) if i.bits == bits && !i.isSigned) {
        Ok(())
    } else {
        Err(failure(
            "pssz dynamic: primitive adapter has incompatible inner schema",
        ))
    }
}
fn bit_shape(schema: &Schema, custom: &crate::pssz_schema_ir::Custom) -> PsszResult<(usize, bool)> {
    let (element, count, list) = match (&custom.id[..], &schema.resolve(&custom.r#type)?.value) {
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

pub fn layout<F: PsszFormat>(schema: &Schema, ty: &AnyType) -> PsszResult<Layout> {
    layout_at::<F>(schema, ty, 0)
}
fn layout_at<F: PsszFormat>(schema: &Schema, ty: &AnyType, n: usize) -> PsszResult<Layout> {
    depth(n)?;
    let ty = schema.resolve(ty)?;
    let mut out = Layout {
        fixed: false,
        size: 0,
        optional: false,
        omit: false,
    };
    match &ty.value {
        AnyValue::Int(i) => {
            out.fixed = true;
            out.size = int_size(i.bits)?;
        }
        AnyValue::Float(f) => {
            out.fixed = true;
            out.size = match (f.exp, f.mantissa) {
                (8, 23 | 24) => 4,
                (11, 52 | 53) => 8,
                _ => return Err(failure("pssz dynamic: unsupported IEEE width")),
            };
        }
        AnyValue::Option(_) => out.optional = true,
        AnyValue::Array(a) => {
            let element = layout_at::<F>(schema, &a.r#type, n + 1)?;
            out.fixed = element.fixed;
            if out.fixed {
                out.size = usize::try_from(a.len)
                    .ok()
                    .and_then(|count| count.checked_mul(element.size))
                    .ok_or(failure("pssz dynamic: array size overflow"))?;
            }
        }
        AnyValue::Struct(s) => return record_layout::<F>(schema, &s.members, true, n),
        AnyValue::Object(o) if dwnc(&o.attributes) => {
            return record_layout::<F>(schema, &o.members, true, n)
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
        AnyValue::Custom(c) if c.id == "string" || c.id == "hex" => {
            return layout_at::<F>(schema, &c.r#type, n + 1)
        }
        AnyValue::Custom(_) => return Err(failure("pssz dynamic: unsupported custom adapter")),
        AnyValue::FracPack(_) => {
            return Err(failure(
                "pssz dynamic: legacy FracPack marker is unsupported",
            ))
        }
        AnyValue::Resource(_) => {
            return Err(failure(
                "pssz dynamic: resource values require an explicit adapter",
            ))
        }
        AnyValue::Type(_) => unreachable!("resolved aliases"),
    }
    Ok(out)
}
fn record_layout<F: PsszFormat>(
    schema: &Schema,
    fields: &[Member],
    stable: bool,
    n: usize,
) -> PsszResult<Layout> {
    let mut out = Layout {
        fixed: stable,
        size: 0,
        optional: false,
        omit: false,
    };
    for field in fields {
        let inner = layout_at::<F>(schema, &field.r#type, n + 1)?;
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
fn can_be_empty<F: PsszFormat>(schema: &Schema, ty: &AnyType, n: usize) -> PsszResult<bool> {
    depth(n)?;
    let ty = schema.resolve(ty)?;
    Ok(match &ty.value {
        AnyValue::List(_) | AnyValue::BoundedList(_) => true,
        AnyValue::Option(o) => !can_be_empty::<F>(schema, &o.r#type, n + 1)?,
        AnyValue::Array(a) => {
            a.len == 0 || {
                let l = layout_at::<F>(schema, &a.r#type, n + 1)?;
                l.fixed && l.size == 0
            }
        }
        AnyValue::Struct(s) => {
            let layout = record_layout::<F>(schema, &s.members, true, n)?;
            layout.fixed && layout.size == 0
        }
        AnyValue::Object(o) if dwnc(&o.attributes) => {
            let layout = record_layout::<F>(schema, &o.members, true, n)?;
            layout.fixed && layout.size == 0
        }
        AnyValue::Custom(c) if c.id == "string" || c.id == "hex" => {
            can_be_empty::<F>(schema, &c.r#type, n + 1)?
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
    let (element, bound, exact) = match &schema.resolve(ty)?.value {
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
    match &schema.resolve(element)?.value {
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

pub fn encode<F: PsszFormat>(schema: &Schema, ty: &AnyType, value: &Value) -> PsszResult<Vec<u8>> {
    checked::format::<F>()?;
    metadata(schema, ty)?;
    encode_at::<F>(schema, ty, value, 0)
}
fn encode_at<F: PsszFormat>(
    schema: &Schema,
    ty: &AnyType,
    value: &Value,
    n: usize,
) -> PsszResult<Vec<u8>> {
    depth(n)?;
    let ty = schema.resolve(ty)?;
    let mut out = Vec::new();
    match (&ty.value, value) {
        (AnyValue::Int(i), Value::Unsigned(v)) if !i.isSigned => {
            let size = int_size(i.bits)?;
            if i.bits < 128 && *v >= (1u128 << i.bits) {
                return Err(failure("pssz dynamic: unsigned integer overflow"));
            }
            out.extend_from_slice(&v.to_le_bytes()[..size]);
        }
        (AnyValue::Int(i), Value::Signed(v)) if i.isSigned => {
            let size = int_size(i.bits)?;
            if i.bits < 128 {
                let limit = 1i128 << (i.bits - 1);
                if *v < -limit || *v >= limit {
                    return Err(failure("pssz dynamic: signed integer overflow"));
                }
            }
            out.extend_from_slice(&v.to_le_bytes()[..size]);
        }
        (AnyValue::Float(f), Value::Float32(v)) if f.exp == 8 && matches!(f.mantissa, 23 | 24) => {
            out.extend_from_slice(&v.to_le_bytes())
        }
        (AnyValue::Float(f), Value::Float64(v)) if f.exp == 11 && matches!(f.mantissa, 52 | 53) => {
            out.extend_from_slice(&v.to_le_bytes())
        }
        (AnyValue::Custom(c), Value::Bool(v)) if c.id == "bool" => {
            unsigned_adapter(schema, &c.r#type, 1)?;
            out.push(u8::from(*v));
        }
        (AnyValue::Custom(c), Value::Unsigned(v)) if c.id == "char" => {
            unsigned_adapter(schema, &c.r#type, 8)?;
            out.push(u8::try_from(*v).map_err(|_| failure("pssz dynamic: char byte overflow"))?);
        }
        (AnyValue::Custom(c), Value::String(v)) if c.id == "string" => {
            bound_bytes(schema, &c.r#type, v.len())?;
            out.extend_from_slice(v.as_bytes());
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
            if can_be_empty::<F>(schema, &o.r#type, 0)? {
                out.push(u8::from(v.is_some()));
            }
            if let Some(inner) = v {
                out.extend(encode_at::<F>(schema, &o.r#type, inner, n + 1)?);
            }
        }
        (AnyValue::Struct(s), Value::Record(values)) => {
            encode_members::<F>(schema, &s.members, values, true, n, &mut out)?
        }
        (AnyValue::Object(o), Value::Record(values)) => {
            encode_members::<F>(schema, &o.members, values, dwnc(&o.attributes), n, &mut out)?
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
                    Ok(layout::<F>(schema, ty)?.optional && matches!(value, Value::Optional(None)))
                })
                .collect::<PsszResult<Vec<_>>>()?;
            let keep = checked::record_kept_fields(&omitted, false);
            for (ty, value) in t.members.iter().zip(values).take(keep) {
                fields.push(encoded_field::<F>(schema, ty, value, n + 1)?);
            }
            checked::encode_record::<F>(&fields, false, &mut out)?;
        }
        (AnyValue::List(l), Value::Sequence(values)) => {
            encode_sequence::<F>(schema, &l.r#type, values, None, false, n, &mut out)?
        }
        (AnyValue::BoundedList(l), Value::Sequence(values)) => encode_sequence::<F>(
            schema,
            &l.r#type,
            values,
            Some(l.maxCount),
            false,
            n,
            &mut out,
        )?,
        (AnyValue::Array(a), Value::Sequence(values)) => {
            encode_sequence::<F>(schema, &a.r#type, values, Some(a.len), true, n, &mut out)?
        }
        (AnyValue::Variant(v), Value::Variant { selector, value }) => {
            if v.members.len() > 256 || *selector >= v.members.len() {
                return Err(failure("pssz dynamic: variant selector out of range"));
            }
            out.push(*selector as u8);
            out.extend(encode_at::<F>(
                schema,
                &v.members[*selector].r#type,
                value,
                n + 1,
            )?);
        }
        (AnyValue::FracPack(_), _) => {
            return Err(failure(
                "pssz dynamic: legacy FracPack marker is unsupported",
            ))
        }
        _ => {
            return Err(failure(
                "pssz dynamic: value does not match supported schema",
            ))
        }
    }
    Ok(out)
}
fn encoded_field<F: PsszFormat>(
    schema: &Schema,
    ty: &AnyType,
    value: &Value,
    n: usize,
) -> PsszResult<(Layout, Vec<u8>)> {
    let mut field = layout::<F>(schema, ty)?;
    field.omit = field.optional && matches!(value, Value::Optional(None));
    let bytes = encode_at::<F>(schema, ty, value, n)?;
    if field.fixed && bytes.len() != field.size {
        return Err(failure("pssz dynamic: fixed field encoding mismatch"));
    }
    Ok((field, bytes))
}
fn encode_members<F: PsszFormat>(
    schema: &Schema,
    members: &[Member],
    values: &[(String, Value)],
    stable: bool,
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
        let value = match values.iter().find(|(name, _)| name == &member.name) {
            Some((_, value)) => value,
            None if layout::<F>(schema, &member.r#type)?.optional => &missing,
            None => return Err(failure("pssz dynamic: missing required value field")),
        };
        omitted.push(
            layout::<F>(schema, &member.r#type)?.optional && matches!(value, Value::Optional(None)),
        );
        child_values.push(value);
    }
    let keep = checked::record_kept_fields(&omitted, stable);
    let mut fields = Vec::new();
    for (member, value) in members.iter().zip(child_values).take(keep) {
        fields.push(encoded_field::<F>(schema, &member.r#type, value, n + 1)?);
    }
    checked::encode_record::<F>(&fields, stable, out)
}
fn encode_sequence<F: PsszFormat>(
    schema: &Schema,
    ty: &AnyType,
    values: &[Value],
    count: Option<u64>,
    exact: bool,
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
    let element = layout::<F>(schema, ty)?;
    if !exact && element.fixed && element.size == 0 && !values.is_empty() {
        return Err(failure("pssz dynamic: ambiguous nonempty zero-size list"));
    }
    let mut fields = Vec::new();
    for value in values {
        fields.push(encoded_field::<F>(schema, ty, value, n + 1)?);
    }
    checked::encode_record::<F>(&fields, true, out)
}

pub fn decode_exact<F: PsszFormat>(
    schema: &Schema,
    ty: &AnyType,
    bytes: &[u8],
) -> PsszResult<Value> {
    metadata(schema, ty)?;
    checked::format::<F>()?;
    let session = checked::DecodeSession::enter(false);
    let value = decode_at::<F>(schema, ty, bytes, 0)?;
    if session.has_unknown() {
        return Err(failure("pssz dynamic: unknown fields in exact decode"));
    }
    Ok(value)
}
pub fn decode_compatible<F: PsszFormat>(
    schema: &Schema,
    ty: &AnyType,
    bytes: &[u8],
) -> PsszResult<Decoded<Value>> {
    metadata(schema, ty)?;
    checked::format::<F>()?;
    let session = checked::DecodeSession::enter(true);
    let value = decode_at::<F>(schema, ty, bytes, 0)?;
    Ok(Decoded {
        value,
        has_unknown: session.has_unknown(),
    })
}
fn decode_at<F: PsszFormat>(
    schema: &Schema,
    ty: &AnyType,
    bytes: &[u8],
    n: usize,
) -> PsszResult<Value> {
    depth(n)?;
    let ty = schema.resolve(ty)?;
    Ok(match &ty.value {
        AnyValue::Int(i) => {
            let size = int_size(i.bits)?;
            if bytes.len() != size {
                return Err(failure("pssz dynamic: integer span mismatch"));
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
        AnyValue::Custom(c) if c.id == "string" => {
            bound_bytes(schema, &c.r#type, bytes.len())?;
            Value::String(
                std::str::from_utf8(bytes)
                    .map_err(|_| failure("pssz dynamic: invalid UTF-8"))?
                    .into(),
            )
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
            if can_be_empty::<F>(schema, &o.r#type, 0)? {
                match bytes.split_first() {
                    Some((&0, [])) => Value::Optional(None),
                    Some((&1, payload)) => Value::Optional(Some(Box::new(decode_at::<F>(
                        schema,
                        &o.r#type,
                        payload,
                        n + 1,
                    )?))),
                    _ => {
                        return Err(failure(
                            "pssz dynamic: invalid optional selector or payload",
                        ))
                    }
                }
            } else if bytes.is_empty() {
                Value::Optional(None)
            } else {
                Value::Optional(Some(Box::new(decode_at::<F>(
                    schema,
                    &o.r#type,
                    bytes,
                    n + 1,
                )?)))
            }
        }
        AnyValue::Struct(s) => {
            Value::Record(decode_members::<F>(schema, &s.members, bytes, true, n)?)
        }
        AnyValue::Object(o) => Value::Record(decode_members::<F>(
            schema,
            &o.members,
            bytes,
            dwnc(&o.attributes),
            n,
        )?),
        AnyValue::Tuple(t) => {
            let layouts = t
                .members
                .iter()
                .map(|ty| layout::<F>(schema, ty))
                .collect::<PsszResult<Vec<_>>>()?;
            let spans = checked::record_spans::<F>(&layouts, bytes, false)?;
            let values = t
                .members
                .iter()
                .zip(spans)
                .map(|(ty, span)| match span {
                    Some(span) => decode_at::<F>(schema, ty, &bytes[span], n + 1),
                    None => Ok(Value::Optional(None)),
                })
                .collect::<PsszResult<Vec<_>>>()?;
            Value::Sequence(values)
        }
        AnyValue::List(l) => Value::Sequence(decode_sequence::<F>(
            schema, &l.r#type, bytes, None, false, n,
        )?),
        AnyValue::BoundedList(l) => Value::Sequence(decode_sequence::<F>(
            schema,
            &l.r#type,
            bytes,
            Some(l.maxCount),
            false,
            n,
        )?),
        AnyValue::Array(a) => Value::Sequence(decode_sequence::<F>(
            schema,
            &a.r#type,
            bytes,
            Some(a.len),
            true,
            n,
        )?),
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
                value: Box::new(decode_at::<F>(schema, &member.r#type, payload, n + 1)?),
            }
        }
        _ => {
            return Err(failure(
                "pssz dynamic: unsupported IR type or custom adapter",
            ))
        }
    })
}
fn decode_members<F: PsszFormat>(
    schema: &Schema,
    members: &[Member],
    bytes: &[u8],
    stable: bool,
    n: usize,
) -> PsszResult<Vec<(String, Value)>> {
    let layouts = members
        .iter()
        .map(|m| layout::<F>(schema, &m.r#type))
        .collect::<PsszResult<Vec<_>>>()?;
    let spans = checked::record_spans::<F>(&layouts, bytes, stable)?;
    members
        .iter()
        .zip(spans)
        .map(|(member, span)| {
            let value = match span {
                Some(span) => decode_at::<F>(schema, &member.r#type, &bytes[span], n + 1)?,
                None => Value::Optional(None),
            };
            Ok((member.name.clone(), value))
        })
        .collect()
}
fn decode_sequence<F: PsszFormat>(
    schema: &Schema,
    ty: &AnyType,
    bytes: &[u8],
    bound: Option<u64>,
    exact: bool,
    n: usize,
) -> PsszResult<Vec<Value>> {
    let element = layout::<F>(schema, ty)?;
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
        let first = checked::read_offset::<F>(bytes)?;
        if first == 0 || first % F::OFFSET_BYTES != 0 || first > bytes.len() {
            return Err(failure("pssz dynamic: variable list table mismatch"));
        }
        first / F::OFFSET_BYTES
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
            .checked_mul(F::OFFSET_BYTES)
            .ok_or(failure("pssz dynamic: list table overflow"))?;
        if table > bytes.len() || (count == 0 && !bytes.is_empty()) {
            return Err(failure("pssz dynamic: list table mismatch"));
        }
        let mut previous = table;
        for i in 0..count {
            let begin = checked::read_offset::<F>(&bytes[i * F::OFFSET_BYTES..])?;
            let end = if i + 1 < count {
                checked::read_offset::<F>(&bytes[(i + 1) * F::OFFSET_BYTES..])?
            } else {
                bytes.len()
            };
            if (i == 0 && begin != table) || begin < previous || begin > end || end > bytes.len() {
                return Err(failure("pssz dynamic: noncanonical list offset"));
            }
            spans.push(begin..end);
            previous = begin;
        }
    }
    spans
        .into_iter()
        .map(|span| decode_at::<F>(schema, ty, &bytes[span], n + 1))
        .collect()
}
