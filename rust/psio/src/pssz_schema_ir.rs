//! Literal structural mirror of C++ `psio/schema_ir.hpp`.
//! This is distinct from the crate-root legacy fracpack schema.
#![allow(non_snake_case)]
use crate::pssz::{PsszError, PsszResult};
use crate::{PsszPack, PsszUnpack};
use serde::{Deserialize, Serialize};
use std::any::TypeId;
use std::collections::HashMap;

macro_rules! ir_record {
    ($name:ident { $($field:ident: $ty:ty),* $(,)? }) => {
        #[derive(Clone, Debug, PartialEq, Serialize, Deserialize, PsszPack, PsszUnpack)]
        #[pssz(crate = "crate")]
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
ir_record!(FracPack { r#type: Box<AnyType> });
ir_record!(Int {
    bits: u32,
    isSigned: bool
});
ir_record!(Float {
    exp: u32,
    mantissa: u32
});
ir_record!(Type { r#type: String });

/// Order is the C++ AnyType::value variant order. Never reorder these tags.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize, PsszPack, PsszUnpack)]
#[pssz(crate = "crate")]
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
    FracPack(FracPack),
    Custom(Custom),
    Resource(Resource),
    Type(Type),
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
ir_record!(Schema { package: Package, interfaces: Vec<Interface>, worlds: Vec<World>, uses: Vec<Use>, types: Vec<Member> });

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
        }
    }
}
impl Schema {
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
                        .ok_or(PsszError("pssz schema: unresolved named type"))?
                }
                _ => return Ok(ty),
            }
        }
        Err(PsszError("pssz schema: cyclic aliases"))
    }
}

pub trait ToPsszSchema: 'static {
    fn schema_name() -> Option<String> {
        None
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
            return Err(PsszError("pssz schema: conflicting named types"));
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
impl ToPsszSchema for String {
    fn build_schema(builder: &mut SchemaBuilder) -> PsszResult<AnyType> {
        Ok(AnyType::new(AnyValue::Custom(Custom {
            r#type: Box::new(AnyType::new(AnyValue::List(List {
                r#type: Box::new(u8::schema(builder)?),
            }))),
            id: "string".into(),
        })))
    }
}
impl<T: ToPsszSchema> ToPsszSchema for Box<T> {
    fn build_schema(builder: &mut SchemaBuilder) -> PsszResult<AnyType> {
        T::build_schema(builder)
    }
    fn schema(builder: &mut SchemaBuilder) -> PsszResult<AnyType> {
        T::schema(builder)
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
        Ok(AnyType::new(AnyValue::BoundedList(BoundedList {
            r#type: Box::new(T::schema(builder)?),
            maxCount: N as u64,
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
            id: "string".into(),
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
