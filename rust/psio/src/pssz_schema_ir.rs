//! Literal structural mirror of C++ `psio/schema_ir.hpp`.
//!
//! Serialized forms of a [`Schema`], each identical to the C++ one: PSSZ
//! (`PsszPack` / `PsszUnpack`) and the two texts of `psio::json`
//! (`JsonText`: `json::try_encode::<IJson, _>(&schema)`,
//! `json::decode::<Json, Schema>(text)`), which are what the generic
//! text codec gives these records (docs/json.md, "The Schema IR as JSON").
//! The serde derives are a separate, serde-shaped form and not that text.
#![allow(non_snake_case)]
use crate::pssz::{PsszError, PsszResult};
use crate::{JsonText, PsszPack, PsszUnpack};
use serde::{Deserialize, Serialize};
use std::any::TypeId;
use std::collections::HashMap;

macro_rules! ir_record {
    ($name:ident { $($field:ident: $ty:ty),* $(,)? }) => {
        #[derive(Clone, Debug, PartialEq, Serialize, Deserialize, PsszPack, PsszUnpack, JsonText)]
        #[psio(crate = "crate")]
        #[psio(crate = "crate")]
        pub struct $name { $(pub $field: $ty),* }
    };
}
ir_record!(Attribute { name: String, value: Option<String> });
ir_record!(Member { name: String, r#type: Box<AnyType>, attributes: Vec<Attribute> });
ir_record!(Func { name: String, params: Vec<Member>, result: Option<Box<AnyType>>, attributes: Vec<Attribute> });
ir_record!(Object { members: Vec<Member>, attributes: Vec<Attribute> });
ir_record!(Struct { members: Vec<Member>, attributes: Vec<Attribute> });
ir_record!(Resource { name: String, methods: Vec<Func>, attributes: Vec<Attribute> });
ir_record!(Array { r#type: Box<AnyType>, len: u64 });
ir_record!(List { r#type: Box<AnyType> });
ir_record!(BoundedList { r#type: Box<AnyType>, maxCount: u64 });
ir_record!(Optional { r#type: Box<AnyType> });
ir_record!(Variant { members: Vec<Member>, attributes: Vec<Attribute> });
ir_record!(Tuple { members: Vec<Box<AnyType>> });
ir_record!(Custom { r#type: Box<AnyType>, id: String });
ir_record!(Nested { r#type: Box<AnyType> });
ir_record!(Int {
    bits: u32,
    isSigned: bool
});
ir_record!(Float {
    exp: u32,
    mantissa: u32
});
ir_record!(Type { r#type: String });
// An enumerator of an `Enum`: `value` is the enumerator's value as 64
// two's-complement bits (sign-extended for a signed underlying type).
ir_record!(Enumerator { name: String, value: u64 });
// An enum: its underlying integer `type` (all there is on the wire) plus the
// enumerator names, which exist for text. A value that is no listed
// enumerator is still a value of the type.
ir_record!(Enum { r#type: Box<AnyType>, members: Vec<Enumerator> });

/// Order is the C++ AnyType::value variant order. Never reorder these tags.
/// The variant identifiers are the alternatives' names in I-JSON text (C++:
/// the reflected name of each alternative).
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize, PsszPack, PsszUnpack, JsonText)]
#[psio(crate = "crate")]
#[psio(crate = "crate")]
pub enum AnyValue {
    Struct(Struct),
    Object(Object),
    Array(Array),
    List(List),
    BoundedList(BoundedList),
    Option(Optional),
    Variant(Variant),
    Tuple(Tuple),
    Int(Int),
    Float(Float),
    Nested(Nested),
    Custom(Custom),
    Resource(Resource),
    Type(Type),
    // Added after the alternatives above; schemas written before it decode
    // unchanged.
    Enum(Enum),
}
// The wrapper is essential: C++ AnyType is a reflected one-field record, not
// a transparent alias for its variant. Its mutable resolution cache is unwired.
ir_record!(AnyType { value: AnyValue });
ir_record!(FunctionType { params: Box<AnyType>, result: Option<Box<AnyType>> });
ir_record!(Package { name: String, version: String, attributes: Vec<Attribute> });
ir_record!(UseItem { name: String, alias: Option<String> });
ir_record!(Use { package: String, interface_name: String, version: String, items: Vec<UseItem> });
ir_record!(UseRef {
    package: String,
    interface_name: String
});
ir_record!(Interface { name: String, type_names: Vec<String>, funcs: Vec<Func>, attributes: Vec<Attribute> });
ir_record!(World { name: String, imports: Vec<UseRef>, exports: Vec<String>, attributes: Vec<Attribute> });
// `ir_version` was appended to the record: a schema written before it
// decodes with `None`, and the schema-driven codecs refuse it (see
// [`IR_VERSION`]).
ir_record!(Schema {
    package: Package,
    interfaces: Vec<Interface>,
    worlds: Vec<World>,
    uses: Vec<Use>,
    types: Vec<Member>,
    ir_version: Option<u32>
});

/// The version of the IR's meaning that this library writes and reads
/// (C++ `psio::schema_ir_version`).
///
/// * 1 — a boxed value is marked `Custom{.., "box"}`, a UTF-8 string is
///   `"utf8"` and `"string"` is a raw string.
///
/// A schema with no `ir_version` predates those markers: read as version 1
/// it would silently derive other offset widths and skip UTF-8 validation,
/// so every schema-driven codec refuses it instead of guessing. `Schema::
/// default()` and `SchemaBuilder` write the current version.
pub const IR_VERSION: u32 = 1;


impl AnyType {
    pub fn new(value: AnyValue) -> Self {
        Self { value }
    }
    pub fn named(name: impl Into<String>) -> Self {
        Self::new(AnyValue::Type(Type {
            r#type: name.into(),
        }))
    }
}
impl Default for Schema {
    fn default() -> Self {
        Self {
            package: Package {
                name: String::new(),
                version: String::new(),
                attributes: vec![],
            },
            interfaces: vec![],
            worlds: vec![],
            uses: vec![],
            types: vec![],
            ir_version: Some(IR_VERSION),
        }
    }
}
impl Schema {
    /// What every schema-driven codec checks first: the schema says which
    /// version of the IR it was written in, and it is the one this library
    /// reads.
    pub fn check_ir_version(&self) -> PsszResult<()> {
        match self.ir_version {
            Some(IR_VERSION) => Ok(()),
            None => Err(PsszError::new(
                "pssz schema: no ir_version marker (the schema predates the box / utf8 markers and \
                 would derive different widths); regenerate it",
            )),
            Some(_) => Err(PsszError::new("pssz schema: ir_version is newer than this library")),
        }
    }
    pub fn get(&self, name: &str) -> Option<&AnyType> {
        self.types
            .iter()
            .find(|m| m.name == name)
            .map(|m| m.r#type.as_ref())
    }
    pub fn resolve<'a>(&'a self, mut ty: &'a AnyType) -> PsszResult<&'a AnyType> {
        for _ in 0..=self.types.len() {
            match &ty.value {
                AnyValue::Type(reference) => {
                    ty = self
                        .get(&reference.r#type)
                        .ok_or(PsszError::new("pssz schema: unresolved named type"))?
                }
                _ => return Ok(ty),
            }
        }
        Err(PsszError::new("pssz schema: cyclic aliases"))
    }
    /// The type as it is on the wire: `resolve`, looking through the nodes
    /// that add nothing to the bytes — an `Enum` (its underlying integer)
    /// and a `Custom` whose id is not part of the binary form (`box`, or a
    /// text presentation; see [`wire_custom_id`]). The binary codecs use
    /// this; text needs the nodes themselves (`resolve`).
    pub fn wire<'a>(&'a self, ty: &'a AnyType) -> PsszResult<&'a AnyType> {
        self.wire_boxed(ty).map(|(ty, _)| ty)
    }
    /// [`Schema::wire`], and whether a `box` was looked through on the way.
    pub fn wire_boxed<'a>(&'a self, mut ty: &'a AnyType) -> PsszResult<(&'a AnyType, bool)> {
        let mut boxed = false;
        // Wrappers between two aliases are finite in a valid schema; the
        // limit only stops a malformed one.
        for _ in 0..=4 * self.types.len() + 64 {
            match &ty.value {
                AnyValue::Type(reference) => {
                    ty = self
                        .get(&reference.r#type)
                        .ok_or(PsszError::new("pssz schema: unresolved named type"))?
                }
                AnyValue::Enum(named) => {
                    self.enum_underlying(&named.r#type)?;
                    ty = &named.r#type
                }
                AnyValue::Custom(custom) if !wire_custom_id(&custom.id) => {
                    boxed |= custom.id == "box";
                    if crate::time::parse_time_schema_id(&custom.id).is_some() {
                        self.time_ticks(&custom.r#type)?;
                    } else if crate::time::reserved_time_schema_id(&custom.id) {
                        return Err(PsszError::new("pssz schema: unsupported custom adapter"));
                    }
                    ty = &custom.r#type;
                }
                _ => return Ok((ty, boxed)),
            }
        }
        Err(PsszError::new("pssz schema: cyclic aliases"))
    }
}

impl Schema {
    /// The type an adapter — a `Custom` id with a binary meaning, a time id —
    /// or an `Enum` sits on: found through named aliases only. A wrapper in
    /// that place (an `Enum`, a `box`, a text presentation) is a malformed
    /// schema in every language, not something to look through.
    pub fn direct<'a>(&'a self, ty: &'a AnyType) -> PsszResult<&'a AnyType> {
        const WRAPPED: PsszError =
            PsszError::new("pssz schema: an adapter or enum must sit directly on its wire type");
        let ty = self.resolve(ty)?;
        match &ty.value {
            AnyValue::Enum(_) => Err(WRAPPED),
            AnyValue::Custom(custom) if !wire_custom_id(&custom.id) => Err(WRAPPED),
            _ => Ok(ty),
        }
    }
    /// The integer under an `Enum`, directly: any integer the codec has
    /// (text needs 8 to 64 bits).
    pub fn enum_underlying<'a>(&'a self, inner: &'a AnyType) -> PsszResult<&'a Int> {
        match &self.direct(inner)?.value {
            AnyValue::Int(int) => Ok(int),
            _ => Err(PsszError::new("pssz schema: an enum needs an underlying integer")),
        }
    }
    /// The tick integer under a time id (`time-point-<unit>` /
    /// `duration-<unit>` / `monotonic-<unit>`, docs/time.md): 8, 16, 32 or 64 bits. Anything
    /// else under a time id is a malformed schema.
    pub fn time_ticks<'a>(&'a self, inner: &'a AnyType) -> PsszResult<&'a Int> {
        match &self.direct(inner)?.value {
            AnyValue::Int(ticks) if matches!(ticks.bits, 8 | 16 | 32 | 64) => Ok(ticks),
            _ => Err(PsszError::new("pssz schema: time value requires an 8/16/32/64-bit tick integer")),
        }
    }
}

// ── Custom ids ──────────────────────────────────────────────────────────────
//
// A Custom node is `type` (what is on the wire) plus an `id`. These ids are
// part of the binary form:
//
//   bool, char           a one-byte primitive over Int{1} / Int{8}
//   string               a RAW string: any bytes (Rust RawString, C++
//                        std::string) over a byte container
//   utf8                 a UTF-8 string (Rust String, C++ utf8_string) over
//                        a byte container: validated on every read and write
//   hex                  bytes over a byte container
//   bitvector, bitlist   packed bits
//   map, set             a list whose entries are in strictly ascending key
//                        order (docs/pssz-spec.md 5.9): `map` directly on a
//                        List / BoundedList of two-member tuples (key,
//                        value), `set` directly on a List / BoundedList
//   box                  the inner type on the wire, with no inferred size
//                        bound (Box<T>): the typed codecs give a boxed value
//                        no bound, so a type holding one gets the same
//                        offset width from the schema
//
// "unknown" marks a C++ type the schema builder could not describe and has
// no codec. Every other id adds nothing to the bytes: on the wire the node is
// exactly its inner type. The time ids are built-in presentations; any other
// id names a custom type (a text form, an invariant, or both) whose handler
// is registered with the codecs (`pssz::dynamic::CustomHandlers`).

/// `string`, `utf8` or `hex`: an adapter over a byte container.
pub fn byte_adapter_id(id: &str) -> bool {
    matches!(id, "string" | "utf8" | "hex")
}
/// `map` or `set`: a list whose entries are in ascending key order.
pub fn assoc_id(id: &str) -> bool {
    matches!(id, "map" | "set")
}
/// An id with a binary meaning of its own. Any other id — `box`, a time id
/// or a custom type — is its inner type on the wire.
pub fn wire_custom_id(id: &str) -> bool {
    matches!(id, "bool" | "char" | "bitvector" | "bitlist" | "unknown")
        || byte_adapter_id(id)
        || assoc_id(id)
}

pub trait ToPsszSchema: 'static {
    fn schema_name() -> Option<String> {
        None
    }
    /// The Custom id of a type with a custom text form
    /// (`#[psio(custom)]` / `#[psio(custom = "Id")]`): its schema is
    /// `Custom { the wire type, id }` and `pssz::dynamic::CustomHandlers`
    /// registers its `TextForm` under this id.
    fn custom_id() -> Option<String> {
        None
    }
    /// Attributes a record member of this type carries: a bound that the IR
    /// records on the member rather than in the type (`lengthBound` of a
    /// bounded nested value).
    fn member_attributes() -> Vec<Attribute> {
        Vec::new()
    }
    fn build_schema(builder: &mut SchemaBuilder) -> PsszResult<AnyType>;
    fn schema(builder: &mut SchemaBuilder) -> PsszResult<AnyType>
    where
        Self: Sized,
    {
        builder.insert::<Self>()
    }
}

#[derive(Default)]
pub struct SchemaBuilder {
    schema: Schema,
    names: HashMap<TypeId, String>,
    owners: HashMap<String, TypeId>,
}
impl SchemaBuilder {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn insert<T: ToPsszSchema>(&mut self) -> PsszResult<AnyType> {
        let id = TypeId::of::<T>();
        if let Some(name) = self.names.get(&id) {
            return Ok(AnyType::named(name.clone()));
        }
        let name = T::schema_name().unwrap_or_else(|| format!("@{}", self.names.len()));
        if self.owners.contains_key(&name) {
            return Err(PsszError::new("pssz schema: conflicting named types"));
        }
        self.names.insert(id, name.clone());
        self.owners.insert(name.clone(), id);
        // Match the C++ builder's declaration order: reserve this slot before
        // walking children, so recursive references and emitted ordering agree.
        let index = self.schema.types.len();
        self.schema.types.push(Member {
            name: name.clone(),
            r#type: Box::new(AnyType::new(AnyValue::Struct(Struct {
                members: vec![],
                attributes: vec![],
            }))),
            attributes: vec![],
        });
        match T::build_schema(self) {
            Ok(ty) => self.schema.types[index].r#type = Box::new(ty),
            Err(error) => {
                self.schema.types.truncate(index);
                let existing = &self.schema.types;
                self.names
                    .retain(|_, name| existing.iter().any(|m| m.name == *name));
                self.owners
                    .retain(|name, _| existing.iter().any(|m| m.name == *name));
                return Err(error);
            }
        }
        Ok(AnyType::named(name))
    }
    pub fn finish(self) -> Schema {
        self.schema
    }
}
pub fn schema_for<T: ToPsszSchema>() -> PsszResult<(Schema, AnyType)> {
    let mut builder = SchemaBuilder::new();
    let root = T::schema(&mut builder)?;
    Ok((builder.finish(), root))
}

macro_rules! schema_integer {
    ($(($ty:ty, $signed:expr)),*) => {$ (
        impl ToPsszSchema for $ty {
            fn build_schema(_: &mut SchemaBuilder) -> PsszResult<AnyType> {
                Ok(AnyType::new(AnyValue::Int(Int { bits: (std::mem::size_of::<Self>() * 8) as u32, isSigned: $signed })))
            }
        }
    )*};
}
schema_integer!(
    (u8, false),
    (u16, false),
    (u32, false),
    (u64, false),
    (u128, false),
    (i8, true),
    (i16, true),
    (i32, true),
    (i64, true),
    (i128, true)
);
impl ToPsszSchema for f32 {
    fn build_schema(_: &mut SchemaBuilder) -> PsszResult<AnyType> {
        Ok(AnyType::new(AnyValue::Float(Float {
            exp: 8,
            mantissa: 24,
        })))
    }
}
impl ToPsszSchema for f64 {
    fn build_schema(_: &mut SchemaBuilder) -> PsszResult<AnyType> {
        Ok(AnyType::new(AnyValue::Float(Float {
            exp: 11,
            mantissa: 53,
        })))
    }
}
impl ToPsszSchema for bool {
    fn build_schema(_: &mut SchemaBuilder) -> PsszResult<AnyType> {
        Ok(AnyType::new(AnyValue::Custom(Custom {
            r#type: Box::new(AnyType::new(AnyValue::Int(Int {
                bits: 1,
                isSigned: false,
            }))),
            id: "bool".into(),
        })))
    }
}
// A UTF-8 string: validated on every read and write (C++ utf8_string).
impl ToPsszSchema for String {
    fn build_schema(builder: &mut SchemaBuilder) -> PsszResult<AnyType> {
        Ok(AnyType::new(AnyValue::Custom(Custom {
            r#type: Box::new(AnyType::new(AnyValue::List(List {
                r#type: Box::new(u8::schema(builder)?),
            }))),
            id: "utf8".into(),
        })))
    }
}
// A box is its inner type on the wire, but the typed codec infers no size
// bound through it (`AUTO_BOUND` is `None`). The marker lets a schema-only
// reader derive the same offset widths.
impl<T: ToPsszSchema> ToPsszSchema for Box<T> {
    fn build_schema(builder: &mut SchemaBuilder) -> PsszResult<AnyType> {
        Ok(AnyType::new(AnyValue::Custom(Custom {
            r#type: Box::new(T::schema(builder)?),
            id: "box".into(),
        })))
    }
}
impl<T: ToPsszSchema> ToPsszSchema for Option<T> {
    fn build_schema(builder: &mut SchemaBuilder) -> PsszResult<AnyType> {
        Ok(AnyType::new(AnyValue::Option(Optional {
            r#type: Box::new(T::schema(builder)?),
        })))
    }
}
impl<T: ToPsszSchema> ToPsszSchema for Vec<T> {
    fn build_schema(builder: &mut SchemaBuilder) -> PsszResult<AnyType> {
        let list = AnyType::new(AnyValue::List(List {
            r#type: Box::new(T::schema(builder)?),
        }));
        Ok(if TypeId::of::<T>() == TypeId::of::<u8>() {
            AnyType::new(AnyValue::Custom(Custom {
                r#type: Box::new(list),
                id: "hex".into(),
            }))
        } else {
            list
        })
    }
}
impl<T: ToPsszSchema, const N: usize> ToPsszSchema for [T; N] {
    fn build_schema(builder: &mut SchemaBuilder) -> PsszResult<AnyType> {
        let array = AnyType::new(AnyValue::Array(Array {
            r#type: Box::new(T::schema(builder)?),
            len: N as u64,
        }));
        Ok(if TypeId::of::<T>() == TypeId::of::<u8>() {
            AnyType::new(AnyValue::Custom(Custom {
                r#type: Box::new(array),
                id: "hex".into(),
            }))
        } else {
            array
        })
    }
}
impl<T: ToPsszSchema, const N: usize> ToPsszSchema for crate::pssz::BoundedList<T, N> {
    fn build_schema(builder: &mut SchemaBuilder) -> PsszResult<AnyType> {
        let list = AnyType::new(AnyValue::BoundedList(BoundedList {
            r#type: Box::new(T::schema(builder)?),
            maxCount: N as u64,
        }));
        // Bytes, like Vec<u8>: one hex string in IJson.
        Ok(if TypeId::of::<T>() == TypeId::of::<u8>() {
            AnyType::new(AnyValue::Custom(Custom {
                r#type: Box::new(list),
                id: "hex".into(),
            }))
        } else {
            list
        })
    }
}
// 32 little-endian bytes; C++ psio::uint256.
impl ToPsszSchema for crate::pssz::Uint256 {
    fn build_schema(_: &mut SchemaBuilder) -> PsszResult<AnyType> {
        Ok(AnyType::new(AnyValue::Int(Int {
            bits: 256,
            isSigned: false,
        })))
    }
}
impl<const N: usize> ToPsszSchema for crate::pssz::BoundedString<N> {
    fn build_schema(builder: &mut SchemaBuilder) -> PsszResult<AnyType> {
        Ok(AnyType::new(AnyValue::Custom(Custom {
            r#type: Box::new(AnyType::new(AnyValue::BoundedList(BoundedList {
                r#type: Box::new(u8::schema(builder)?),
                maxCount: N as u64,
            }))),
            id: "utf8".into(),
        })))
    }
}
// Raw byte strings (C++ std::string) carry no UTF-8 requirement: the
// "string" adapter, any bytes. A UTF-8 string (Rust String, C++ utf8_string)
// is "utf8". Byte lists and arrays are "hex".
impl ToPsszSchema for crate::pssz::RawString {
    fn build_schema(builder: &mut SchemaBuilder) -> PsszResult<AnyType> {
        Ok(AnyType::new(AnyValue::Custom(Custom {
            r#type: Box::new(AnyType::new(AnyValue::List(List {
                r#type: Box::new(u8::schema(builder)?),
            }))),
            id: "string".into(),
        })))
    }
}
impl<const N: usize> ToPsszSchema for crate::pssz::BoundedRawString<N> {
    fn build_schema(builder: &mut SchemaBuilder) -> PsszResult<AnyType> {
        Ok(AnyType::new(AnyValue::Custom(Custom {
            r#type: Box::new(AnyType::new(AnyValue::BoundedList(BoundedList {
                r#type: Box::new(u8::schema(builder)?),
                maxCount: N as u64,
            }))),
            id: "string".into(),
        })))
    }
}
impl<const N: usize> ToPsszSchema for crate::pssz::Bitvector<N> {
    fn build_schema(_: &mut SchemaBuilder) -> PsszResult<AnyType> {
        Ok(AnyType::new(AnyValue::Custom(Custom {
            r#type: Box::new(AnyType::new(AnyValue::Array(Array {
                r#type: Box::new(AnyType::new(AnyValue::Int(Int {
                    bits: 1,
                    isSigned: false,
                }))),
                len: N as u64,
            }))),
            id: "bitvector".into(),
        })))
    }
}
impl<const N: usize> ToPsszSchema for crate::pssz::Bitlist<N> {
    fn build_schema(_: &mut SchemaBuilder) -> PsszResult<AnyType> {
        Ok(AnyType::new(AnyValue::Custom(Custom {
            r#type: Box::new(AnyType::new(AnyValue::BoundedList(BoundedList {
                r#type: Box::new(AnyType::new(AnyValue::Int(Int {
                    bits: 1,
                    isSigned: false,
                }))),
                maxCount: N as u64,
            }))),
            id: "bitlist".into(),
        })))
    }
}
macro_rules! schema_tuple {
    ($($ty:ident),*) => {
        impl<$($ty: ToPsszSchema),*> ToPsszSchema for ($($ty,)*) {
            fn build_schema(builder: &mut SchemaBuilder) -> PsszResult<AnyType> {
                let _ = &builder;
                Ok(AnyType::new(AnyValue::Tuple(Tuple { members: vec![$(Box::new($ty::schema(builder)?)),*] })))
            }
        }
    };
}
schema_tuple!();
schema_tuple!(A);
schema_tuple!(A, B);
schema_tuple!(A, B, C);
schema_tuple!(A, B, C, D);
schema_tuple!(A, B, C, D, E);
schema_tuple!(A, B, C, D, E, F);
schema_tuple!(A, B, C, D, E, F, G);
schema_tuple!(A, B, C, D, E, F, G, H);
schema_tuple!(A, B, C, D, E, F, G, H, I);
schema_tuple!(A, B, C, D, E, F, G, H, I, J);
schema_tuple!(A, B, C, D, E, F, G, H, I, J, K);
schema_tuple!(A, B, C, D, E, F, G, H, I, J, K, L);
