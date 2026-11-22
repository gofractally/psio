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
//! Attributes read by the codec:
//!   Object  definitionWillNotChange    fixed layout (no header)
//!           maxDynamicData = "N"       explicit cap (automatic profile)
//!   Member  lengthBound = "max=N" | "exact=N" | "min=N" (comma-separated)
//!           jsonInt / ijsonInt = "number" | "string"   (`json` submodule)
//!   Variant member  ijsonName = "..."                   (`json` submodule)
//! The JSON / I-JSON text path over the same IR is the `json` submodule.
use crate::pssz::{Decoded, Pssz, PsszError, PsszFormat, PsszResult};
use crate::pssz_checked::{self as checked, Layout};
use crate::pssz_schema_ir::{AnyType, AnyValue, Attribute, Member, Schema};
use serde::{Deserialize, Serialize};
use std::cell::RefCell;
use std::collections::{HashMap, HashSet};

pub mod json;
pub use json::{
    json_to_pssz, json_to_value, pssz_to_json, read_json, validate_pssz, value_to_json,
    SchemaJsonError,
};

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
    const MALFORMED: PsszError = PsszError("pssz dynamic: malformed lengthBound attribute");
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
#[derive(Clone, Copy, PartialEq, Eq)]
enum Site {
    /// Object / Struct
    Record,
    RecordMember,
    Variant,
    VariantMember,
}
/// What the explicit profile says about a constraint it does not enforce.
pub const UNSUPPORTED_CONSTRAINT: &str =
    "pssz dynamic: unsupported schema constraint or annotation";
/// `caps`: the automatic profile, which implements maxDynamicData. The
/// explicit profile rejects the constraints it does not enforce.
fn attributes(values: &[Attribute], site: Site, caps: bool) -> PsszResult<()> {
    for value in values {
        match value.name.as_str() {
            "doc" | "description" | "visible" => (),
            "definitionWillNotChange" if site == Site::Record => (),
            "maxDynamicData" | "maxFields" if site == Site::Record && caps => (),
            "lengthBound" if site == Site::RecordMember => (),
            "jsonInt" | "ijsonInt" if site == Site::RecordMember => {
                if !matches!(value.value.as_deref(), Some("number" | "string")) {
                    return Err(failure("pssz dynamic: malformed integer form attribute"));
                }
            }
            "ijsonName" if site == Site::VariantMember && value.value.is_some() => (),
            _ => return Err(failure(UNSUPPORTED_CONSTRAINT)),
        }
    }
    if site == Site::RecordMember {
        parse_length_bound(values)?;
    }
    if site == Site::Record && caps {
        parse_cap(values)?;
    }
    Ok(())
}
/// Checks every type reachable from `root`. `caps` is the automatic profile.
pub(crate) fn metadata(schema: &Schema, root: &AnyType, caps: bool) -> PsszResult<()> {
    let mut pending = vec![root];
    let mut seen = HashSet::new();
    while let Some(ty) = pending.pop() {
        let ty = schema.resolve(ty)?;
        if !seen.insert(ty as *const AnyType as usize) {
            continue;
        }
        let fields = match &ty.value {
            AnyValue::Object(o) => {
                attributes(&o.attributes, Site::Record, caps)?;
                Some((o.members.as_slice(), Site::RecordMember))
            }
            AnyValue::Struct(s) => {
                attributes(&s.attributes, Site::Record, caps)?;
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

/// How a value of `ty` sits in a record's fixed region. The same under every
/// profile: no width enters it.
pub fn layout<F: PsszFormat>(schema: &Schema, ty: &AnyType) -> PsszResult<Layout> {
    layout_at(schema, ty, 0)
}
fn layout_of(schema: &Schema, ty: &AnyType) -> PsszResult<Layout> {
    layout_at(schema, ty, 0)
}
fn layout_at(schema: &Schema, ty: &AnyType, n: usize) -> PsszResult<Layout> {
    depth(n)?;
    let ty = schema.resolve(ty)?;
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
        AnyValue::Custom(c) if c.id == "string" || c.id == "hex" => {
            return layout_at(schema, &c.r#type, n + 1)
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
    let ty = schema.resolve(ty)?;
    Ok(match &ty.value {
        AnyValue::List(_) | AnyValue::BoundedList(_) => true,
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
        AnyValue::Custom(c) if c.id == "string" || c.id == "hex" => {
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
    /// Widths already derived for a container type (automatic profile),
    /// keyed by the resolved type's address in `schema`.
    widths: RefCell<HashMap<usize, usize>>,
}

impl<'a> Codec<'a> {
    fn new<F: PsszFormat>(schema: &'a Schema) -> PsszResult<Self> {
        checked::format::<F>()?;
        Ok(Self {
            schema,
            w: if F::AUTO { 0 } else { F::OFFSET_BYTES },
            widths: RefCell::new(HashMap::new()),
        })
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
        let ty = self.schema.resolve(input)?;
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
            AnyValue::Custom(c) if c.id == "bool" || c.id == "char" => true,
            AnyValue::Custom(c) if c.id == "hex" || c.id == "string" => {
                let child = self.schema.resolve(&c.r#type)?;
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
    /// unbounded. Bookkeeping is four bytes per header and per offset slot,
    /// whatever width is finally selected.
    fn bound_of(
        &self,
        input: &AnyType,
        annotation: Option<&LengthBound>,
        reserve_cap: bool,
        active: &mut Vec<usize>,
    ) -> PsszResult<Bound> {
        let ty = self.schema.resolve(input)?;
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
    /// N elements, each its reserved payload plus an offset slot when it is
    /// not an inline fixed payload.
    fn bound_elements(
        &self,
        element: &AnyType,
        count: u64,
        active: &mut Vec<usize>,
    ) -> PsszResult<Bound> {
        if count == 0 {
            return Ok(Some(0));
        }
        let mut each = self.bound_of(element, None, true, active)?;
        if !self.fixed_payload(element, &mut Vec::new())? {
            each = bound_add(each, 4);
        }
        Ok(bound_multiply(count, each))
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
        let record = |members: &[Member], stable: bool, active: &mut Vec<usize>| {
            let mut total: Bound = Some(if stable { 0 } else { 4 });
            for member in members {
                let bound = parse_length_bound(&member.attributes)?;
                let mut payload = self.bound_of(&member.r#type, bound.as_ref(), true, active)?;
                if !self.fixed_payload(&member.r#type, &mut Vec::new())? {
                    payload = bound_add(payload, 4);
                }
                total = match (total, payload) {
                    (Some(total), Some(payload)) => total.checked_add(payload),
                    _ => None,
                };
            }
            Ok(total)
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
            AnyValue::Custom(c) if c.id == "string" || c.id == "hex" => {
                self.bound_sequence(self.schema.resolve(&c.r#type)?, annotation, active)
            }
            AnyValue::Struct(s) => record(&s.members, true, active),
            AnyValue::Object(o) => record(&o.members, dwnc(&o.attributes), active),
            // Bit types, tuples, variants: no inferred bound.
            _ => Ok(None),
        }
    }
    /// The width of a record's header and slots, or of a tuple / list /
    /// array offset table. `ty` is resolved. Explicit profile: the profile's
    /// width. Automatic: the type's own.
    fn width_of(&self, ty: &AnyType) -> PsszResult<usize> {
        if !self.automatic() {
            return Ok(self.w);
        }
        let key = ty as *const AnyType as usize;
        if let Some(width) = self.widths.borrow().get(&key) {
            return Ok(*width);
        }
        // A list's offset table is always 4 wide (a Vec has no bound of its
        // own; a bounded list keeps the Vec's width). Tuples and variants
        // have no inferred bound: 4.
        let width = if matches!(ty.value, AnyValue::List(_) | AnyValue::BoundedList(_)) {
            4
        } else {
            let bound = match self.cap_of(ty)? {
                Some(cap) => usize::try_from(cap).ok(),
                None => self.bound_of(ty, None, false, &mut Vec::new())?,
            };
            crate::pssz::width_class(bound)
        };
        self.widths.borrow_mut().insert(key, width);
        Ok(width)
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

    fn encode_at(&self, ty: &AnyType, value: &Value, n: usize) -> PsszResult<Vec<u8>> {
        depth(n)?;
        let schema = self.schema;
        let ty = schema.resolve(ty)?;
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
                if can_be_empty(schema, &o.r#type, 0)? {
                    out.push(u8::from(v.is_some()));
                }
                if let Some(inner) = v {
                    out.extend(self.encode_at(&o.r#type, inner, n + 1)?);
                }
            }
            (AnyValue::Struct(s), Value::Record(values)) => {
                let w = self.width_of(ty)?;
                self.encode_members(&s.members, values, true, w, n, &mut out)?;
                self.check_cap(ty, out.len())?;
            }
            (AnyValue::Object(o), Value::Record(values)) => {
                let w = self.width_of(ty)?;
                self.encode_members(&o.members, values, dwnc(&o.attributes), w, n, &mut out)?;
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
                checked::encode_record_w(self.width_of(ty)?, &fields, false, &mut out)?;
            }
            (AnyValue::List(l), Value::Sequence(values)) => {
                let w = self.width_of(ty)?;
                self.encode_sequence(&l.r#type, values, None, false, w, n, &mut out)?
            }
            (AnyValue::BoundedList(l), Value::Sequence(values)) => {
                let w = self.width_of(ty)?;
                self.encode_sequence(&l.r#type, values, Some(l.maxCount), false, w, n, &mut out)?
            }
            (AnyValue::Array(a), Value::Sequence(values)) => {
                let w = self.width_of(ty)?;
                self.encode_sequence(&a.r#type, values, Some(a.len), true, w, n, &mut out)?
            }
            (AnyValue::Variant(v), Value::Variant { selector, value }) => {
                if v.members.len() > 256 || *selector >= v.members.len() {
                    return Err(failure("pssz dynamic: variant selector out of range"));
                }
                out.push(*selector as u8);
                out.extend(self.encode_at(&v.members[*selector].r#type, value, n + 1)?);
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
            if parse_length_bound(&member.attributes)?.is_some_and(|b| !b.admits_value(value)) {
                return Err(failure("pssz dynamic: member length bound violated"));
            }
            omitted.push(optional && matches!(value, Value::Optional(None)));
            child_values.push(value);
        }
        let keep = checked::record_kept_fields(&omitted, stable);
        let mut fields = Vec::new();
        for (member, value) in members.iter().zip(child_values).take(keep) {
            fields.push(self.encoded_field(&member.r#type, value, n + 1)?);
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

    fn decode_at(&self, ty: &AnyType, bytes: &[u8], n: usize) -> PsszResult<Value> {
        depth(n)?;
        let schema = self.schema;
        let ty = schema.resolve(ty)?;
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
                if can_be_empty(schema, &o.r#type, 0)? {
                    match bytes.split_first() {
                        Some((&0, [])) => Value::Optional(None),
                        Some((&1, payload)) => Value::Optional(Some(Box::new(
                            self.decode_at(&o.r#type, payload, n + 1)?,
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
                    Value::Optional(Some(Box::new(self.decode_at(&o.r#type, bytes, n + 1)?)))
                }
            }
            AnyValue::Struct(s) => {
                self.check_cap(ty, bytes.len())?;
                let w = self.width_of(ty)?;
                Value::Record(self.decode_members(&s.members, bytes, true, w, n)?)
            }
            AnyValue::Object(o) => {
                self.check_cap(ty, bytes.len())?;
                let w = self.width_of(ty)?;
                Value::Record(self.decode_members(&o.members, bytes, dwnc(&o.attributes), w, n)?)
            }
            AnyValue::Tuple(t) => {
                let layouts = t
                    .members
                    .iter()
                    .map(|ty| layout_of(schema, ty))
                    .collect::<PsszResult<Vec<_>>>()?;
                let spans = checked::record_spans_w(self.width_of(ty)?, &layouts, bytes, false)?;
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
                let w = self.width_of(ty)?;
                Value::Sequence(self.decode_sequence(&l.r#type, bytes, None, false, w, n)?)
            }
            AnyValue::BoundedList(l) => {
                let w = self.width_of(ty)?;
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
                let w = self.width_of(ty)?;
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
                let value = match span {
                    Some(span) => self.decode_at(&member.r#type, &bytes[span], n + 1)?,
                    None => Value::Optional(None),
                };
                if parse_length_bound(&member.attributes)?.is_some_and(|b| !b.admits_value(&value))
                {
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
    let codec = Codec::new::<F>(schema)?;
    codec.check(ty)?;
    codec.encode_at(ty, value, 0)
}
/// Rejects unknown appended optional fields.
pub fn decode_exact<F: PsszFormat>(
    schema: &Schema,
    ty: &AnyType,
    bytes: &[u8],
) -> PsszResult<Value> {
    let codec = Codec::new::<F>(schema)?;
    codec.check(ty)?;
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
    let codec = Codec::new::<F>(schema)?;
    codec.check(ty)?;
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
    decode_compatible::<Pssz>(schema, ty, bytes).map(|decoded| decoded.has_unknown)
}

/// The offset width the automatic profile derives for `ty`'s own header,
/// slots or offset table: what `psio::pssz::auto_pssz_width::<T>()` is for
/// the typed codec.
pub fn auto_width(schema: &Schema, ty: &AnyType) -> PsszResult<usize> {
    let codec = Codec::new::<Pssz>(schema)?;
    codec.check(ty)?;
    codec.width_of(schema.resolve(ty)?)
}
