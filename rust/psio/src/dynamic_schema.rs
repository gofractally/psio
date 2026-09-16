//! Dynamic schema types for runtime type introspection.
//!
//! Provides `DynamicType`, `FieldDesc`, `AltDesc`, `DynamicSchema`, and `FieldName`
//! for format-agnostic runtime field access.
//!
//! Ported from the C++ `psio` dynamic schema types in `capnp_view.hpp`.
//! These are pure data structures with no format dependency.

use super::xxh64;

// ---------------------------------------------------------------------------
// DynamicType -- type tag for dynamically-accessed values
// ---------------------------------------------------------------------------

/// Type tag for dynamically-accessed values.
///
/// Covers all scalar types, strings, bytes, containers (vector, struct, variant),
/// and a void sentinel for absent/uninitialized values.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum DynamicType {
    Void,
    Bool,
    I8,
    I16,
    I32,
    I64,
    U8,
    U16,
    U32,
    U64,
    F32,
    F64,
    /// UTF-8 text (Cap'n Proto `Text`, fracpack `String`, etc.)
    Text,
    /// Raw byte blob
    Data,
    /// Homogeneous list/vector
    Vector,
    /// Struct (product type)
    Struct,
    /// Variant/union (sum type)
    Variant,
}

impl DynamicType {
    /// Size in bytes for scalar types. Returns 0 for non-scalar types and Bool
    /// (which is sub-byte).
    pub const fn byte_size(self) -> u8 {
        match self {
            Self::Bool => 0,
            Self::I8 | Self::U8 => 1,
            Self::I16 | Self::U16 => 2,
            Self::I32 | Self::U32 | Self::F32 => 4,
            Self::I64 | Self::U64 | Self::F64 => 8,
            _ => 0,
        }
    }

    /// Whether this type is stored as a pointer (rather than inline data).
    pub const fn is_pointer(self) -> bool {
        matches!(
            self,
            Self::Text | Self::Data | Self::Vector | Self::Struct
        )
    }

    /// Whether this type is a scalar (inline fixed-size value).
    pub const fn is_scalar(self) -> bool {
        matches!(
            self,
            Self::Bool
                | Self::I8
                | Self::I16
                | Self::I32
                | Self::I64
                | Self::U8
                | Self::U16
                | Self::U32
                | Self::U64
                | Self::F32
                | Self::F64
        )
    }
}

// ---------------------------------------------------------------------------
// DynamicTypeInfo -- rich type descriptor returned by dynamic_view::type()
// ---------------------------------------------------------------------------

/// Rich type descriptor returned by runtime type queries.
///
/// For variant fields, `kind` is `DynamicType::Variant`, `active_kind` holds the
/// type of the currently-active alternative, and `variant_index` identifies which
/// alternative is active.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DynamicTypeInfo {
    pub kind: DynamicType,
    /// For variants: the type of the active alternative.
    pub active_kind: DynamicType,
    /// For variants: index of the active alternative.
    pub variant_index: u8,
    /// For scalars: the byte size (0 for pointer types / bool).
    pub byte_size: u8,
}

impl Default for DynamicTypeInfo {
    fn default() -> Self {
        Self {
            kind: DynamicType::Void,
            active_kind: DynamicType::Void,
            variant_index: 0,
            byte_size: 0,
        }
    }
}

// ---------------------------------------------------------------------------
// FieldName -- name + pre-computed xxh64 hash
// ---------------------------------------------------------------------------

/// Field name with pre-computed xxh64 hash.
///
/// Pre-computing the hash avoids redundant hashing on repeated lookups.
/// The `tag()` method returns the low byte used for fast scanning.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FieldName {
    pub name: String,
    pub hash: u64,
}

impl FieldName {
    /// Create a `FieldName`, computing the xxh64 hash of `name`.
    pub fn new(name: &str) -> Self {
        Self {
            name: name.to_owned(),
            hash: xxh64::hash_str(name),
        }
    }

    /// Create from a name and a pre-computed hash.
    pub const fn with_hash(name: String, hash: u64) -> Self {
        Self { name, hash }
    }

    /// The low byte of the hash, used for fast tag scanning.
    pub const fn tag(&self) -> u8 {
        self.hash as u8
    }
}

impl From<&str> for FieldName {
    fn from(s: &str) -> Self {
        Self::new(s)
    }
}

// ---------------------------------------------------------------------------
// AltDesc -- per-alternative descriptor for variants
// ---------------------------------------------------------------------------

/// Descriptor for a single alternative within a variant/union field.
///
/// Mirrors the C++ `dynamic_alt_desc`.
#[derive(Debug, Clone)]
pub struct AltDesc {
    /// Type of this alternative (`Void` for a monostate/unit alternative).
    pub ty: DynamicType,
    /// Whether this alternative is stored as a pointer.
    pub is_ptr: bool,
    /// Byte offset in the data section (for inline scalars).
    pub offset: u32,
    /// Bit index within the offset byte (for Bool alternatives).
    pub bit_index: u8,
    /// Scalar size in bytes (0 for pointer types / bool).
    pub byte_size: u8,
    /// Nested schema for struct-typed alternatives, if any.
    pub nested: Option<Box<DynamicSchema>>,
}

impl Default for AltDesc {
    fn default() -> Self {
        Self {
            ty: DynamicType::Void,
            is_ptr: false,
            offset: 0,
            bit_index: 0,
            byte_size: 0,
            nested: None,
        }
    }
}

// ---------------------------------------------------------------------------
// FieldDesc -- per-field descriptor
// ---------------------------------------------------------------------------

/// Descriptor for a single field within a struct schema.
///
/// Mirrors the C++ `dynamic_field_desc`. Fields are stored sorted by `name_hash`
/// within the parent `DynamicSchema` for fast lookup.
#[derive(Debug, Clone)]
pub struct FieldDesc {
    /// xxh64 hash of the field name.
    pub name_hash: u64,
    /// Original field name (for collision verification and debugging).
    pub name: String,
    /// Type of this field.
    pub ty: DynamicType,
    /// Whether this field is stored as a pointer.
    pub is_ptr: bool,
    /// Byte offset in data section, or pointer index.
    pub offset: u32,
    /// Bit index within the offset byte (for Bool fields).
    pub bit_index: u8,
    /// Scalar size in bytes (0 for pointer types / bool).
    pub byte_size: u8,
    /// Nested schema for struct-typed fields or struct vector elements.
    pub nested: Option<Box<DynamicSchema>>,
    /// Alternative descriptors (for variant/union fields).
    pub alternatives: Vec<AltDesc>,
    /// Discriminant byte offset (for variant fields).
    pub disc_offset: u32,
}

impl FieldDesc {
    /// Create a scalar field descriptor.
    pub fn scalar(name: &str, ty: DynamicType, offset: u32) -> Self {
        Self {
            name_hash: xxh64::hash_str(name),
            name: name.to_owned(),
            ty,
            is_ptr: false,
            offset,
            bit_index: 0,
            byte_size: ty.byte_size(),
            nested: None,
            alternatives: Vec::new(),
            disc_offset: 0,
        }
    }

    /// Create a bool field descriptor with a specific bit index.
    pub fn bool_field(name: &str, offset: u32, bit_index: u8) -> Self {
        Self {
            name_hash: xxh64::hash_str(name),
            name: name.to_owned(),
            ty: DynamicType::Bool,
            is_ptr: false,
            offset,
            bit_index,
            byte_size: 0,
            nested: None,
            alternatives: Vec::new(),
            disc_offset: 0,
        }
    }

    /// Create a pointer field descriptor (text, data, struct, vector).
    pub fn pointer(name: &str, ty: DynamicType, ptr_index: u32) -> Self {
        Self {
            name_hash: xxh64::hash_str(name),
            name: name.to_owned(),
            ty,
            is_ptr: true,
            offset: ptr_index,
            bit_index: 0,
            byte_size: 0,
            nested: None,
            alternatives: Vec::new(),
            disc_offset: 0,
        }
    }

    /// Create a struct field descriptor with a nested schema.
    pub fn struct_field(name: &str, ptr_index: u32, nested: DynamicSchema) -> Self {
        Self {
            name_hash: xxh64::hash_str(name),
            name: name.to_owned(),
            ty: DynamicType::Struct,
            is_ptr: true,
            offset: ptr_index,
            bit_index: 0,
            byte_size: 0,
            nested: Some(Box::new(nested)),
            alternatives: Vec::new(),
            disc_offset: 0,
        }
    }

    /// Create a variant field descriptor.
    pub fn variant(
        name: &str,
        disc_offset: u32,
        alternatives: Vec<AltDesc>,
    ) -> Self {
        Self {
            name_hash: xxh64::hash_str(name),
            name: name.to_owned(),
            ty: DynamicType::Variant,
            is_ptr: false,
            offset: 0,
            bit_index: 0,
            byte_size: 0,
            nested: None,
            alternatives,
            disc_offset,
        }
    }

    /// The low byte of the hash, used for tag scanning.
    pub const fn tag(&self) -> u8 {
        self.name_hash as u8
    }
}

impl Default for FieldDesc {
    fn default() -> Self {
        Self {
            name_hash: 0,
            name: String::new(),
            ty: DynamicType::Void,
            is_ptr: false,
            offset: 0,
            bit_index: 0,
            byte_size: 0,
            nested: None,
            alternatives: Vec::new(),
            disc_offset: 0,
        }
    }
}

// ---------------------------------------------------------------------------
// DynamicSchema -- per-type schema with hash-sorted field table
// ---------------------------------------------------------------------------

/// Maximum number of fields for which tag-byte scanning is used.
///
/// Structs with more fields can fall back to binary search on hash.
/// Most structs have fewer than 32 fields, so a single SIMD load covers them.
pub const MAX_SIMD_FIELDS: usize = 64;

/// Per-type dynamic schema: hash-sorted field table with tag bytes for fast lookup.
///
/// Mirrors the C++ `dynamic_schema`. Fields are sorted by `name_hash`; the
/// parallel `tags` vector stores the low byte of each hash for SIMD-friendly
/// scanning.
///
/// # Lookup strategy
///
/// `find()` does a linear scan over the `tags` array (the low byte of each
/// field's xxh64 hash). On a tag match it verifies the full 64-bit hash and
/// the name string. For typical struct sizes (< 32 fields) this fits in one
/// or two SIMD loads and is effectively O(1).
#[derive(Debug, Clone)]
pub struct DynamicSchema {
    /// Fields sorted by `name_hash`.
    pub sorted_fields: Vec<FieldDesc>,
    /// Low byte of each field's hash, parallel to `sorted_fields`.
    pub tags: Vec<u8>,
    /// Field names in original declaration order (for ordered iteration).
    pub ordered_names: Vec<String>,
    /// Number of 8-byte data words in the struct's data section.
    pub data_words: u16,
    /// Number of pointer slots in the struct's pointer section.
    pub ptr_count: u16,
}

impl DynamicSchema {
    /// Build a schema from an unsorted list of field descriptors.
    ///
    /// Fields are cloned, sorted by `name_hash`, and the parallel `tags` array
    /// is computed. `ordered_names` preserves declaration order.
    pub fn new(
        fields: Vec<FieldDesc>,
        data_words: u16,
        ptr_count: u16,
    ) -> Self {
        let ordered_names: Vec<String> =
            fields.iter().map(|f| f.name.clone()).collect();

        let mut sorted = fields;
        sorted.sort_by_key(|f| f.name_hash);

        let tags: Vec<u8> = sorted.iter().map(|f| f.tag()).collect();

        Self {
            sorted_fields: sorted,
            tags,
            ordered_names,
            data_words,
            ptr_count,
        }
    }

    /// Number of fields.
    pub fn field_count(&self) -> usize {
        self.sorted_fields.len()
    }

    /// Find a field by pre-computed hash and name (for collision safety).
    ///
    /// Scans the tag bytes (low byte of hash) linearly, then verifies the full
    /// 64-bit hash and name string on a tag match.
    pub fn find(&self, hash: u64, name: &str) -> Option<&FieldDesc> {
        let tag = hash as u8;
        for (i, &t) in self.tags.iter().enumerate() {
            if t == tag {
                let field = &self.sorted_fields[i];
                if field.name_hash == hash && field.name == name {
                    return Some(field);
                }
            }
        }
        None
    }

    /// Find a field by `FieldName` (has pre-computed hash).
    pub fn find_by_field_name(&self, fn_: &FieldName) -> Option<&FieldDesc> {
        self.find(fn_.hash, &fn_.name)
    }

    /// Find a field by name (computes hash on the fly).
    pub fn find_by_name(&self, name: &str) -> Option<&FieldDesc> {
        let hash = xxh64::hash_str(name);
        self.find(hash, name)
    }

    /// Returns an iterator over field descriptors in declaration order.
    pub fn fields_ordered(&self) -> impl Iterator<Item = &FieldDesc> {
        self.ordered_names.iter().map(move |name| {
            self.find_by_name(name)
                .expect("ordered_names contains a name not in sorted_fields")
        })
    }
}

impl Default for DynamicSchema {
    fn default() -> Self {
        Self {
            sorted_fields: Vec::new(),
            tags: Vec::new(),
            ordered_names: Vec::new(),
            data_words: 0,
            ptr_count: 0,
        }
    }
}

// ---------------------------------------------------------------------------
// SchemaBuilder -- convenience builder for constructing schemas
// ---------------------------------------------------------------------------

/// Builder for constructing a `DynamicSchema` incrementally.
///
/// ```
/// use psio::dynamic_schema::{SchemaBuilder, DynamicType};
///
/// let schema = SchemaBuilder::new()
///     .field_scalar("x", DynamicType::I32, 0)
///     .field_scalar("y", DynamicType::I32, 4)
///     .data_words(1)
///     .build();
///
/// assert_eq!(schema.field_count(), 2);
/// assert!(schema.find_by_name("x").is_some());
/// ```
pub struct SchemaBuilder {
    fields: Vec<FieldDesc>,
    data_words: u16,
    ptr_count: u16,
}

impl SchemaBuilder {
    pub fn new() -> Self {
        Self {
            fields: Vec::new(),
            data_words: 0,
            ptr_count: 0,
        }
    }

    /// Add a scalar field.
    pub fn field_scalar(mut self, name: &str, ty: DynamicType, offset: u32) -> Self {
        self.fields.push(FieldDesc::scalar(name, ty, offset));
        self
    }

    /// Add a bool field.
    pub fn field_bool(mut self, name: &str, offset: u32, bit_index: u8) -> Self {
        self.fields.push(FieldDesc::bool_field(name, offset, bit_index));
        self
    }

    /// Add a pointer field (text, data, vector, struct).
    pub fn field_pointer(mut self, name: &str, ty: DynamicType, ptr_index: u32) -> Self {
        self.fields.push(FieldDesc::pointer(name, ty, ptr_index));
        self
    }

    /// Add a struct field with a nested schema.
    pub fn field_struct(mut self, name: &str, ptr_index: u32, nested: DynamicSchema) -> Self {
        self.fields.push(FieldDesc::struct_field(name, ptr_index, nested));
        self
    }

    /// Add a variant field.
    pub fn field_variant(
        mut self,
        name: &str,
        disc_offset: u32,
        alternatives: Vec<AltDesc>,
    ) -> Self {
        self.fields.push(FieldDesc::variant(name, disc_offset, alternatives));
        self
    }

    /// Add a pre-built field descriptor.
    pub fn field(mut self, desc: FieldDesc) -> Self {
        self.fields.push(desc);
        self
    }

    /// Set the number of data words.
    pub fn data_words(mut self, dw: u16) -> Self {
        self.data_words = dw;
        self
    }

    /// Set the number of pointer slots.
    pub fn ptr_count(mut self, pc: u16) -> Self {
        self.ptr_count = pc;
        self
    }

    /// Consume the builder and produce a `DynamicSchema`.
    pub fn build(self) -> DynamicSchema {
        DynamicSchema::new(self.fields, self.data_words, self.ptr_count)
    }
}

impl Default for SchemaBuilder {
    fn default() -> Self {
        Self::new()
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dynamic_type_byte_sizes() {
        assert_eq!(DynamicType::Void.byte_size(), 0);
        assert_eq!(DynamicType::Bool.byte_size(), 0);
        assert_eq!(DynamicType::I8.byte_size(), 1);
        assert_eq!(DynamicType::U8.byte_size(), 1);
        assert_eq!(DynamicType::I16.byte_size(), 2);
        assert_eq!(DynamicType::U16.byte_size(), 2);
        assert_eq!(DynamicType::I32.byte_size(), 4);
        assert_eq!(DynamicType::U32.byte_size(), 4);
        assert_eq!(DynamicType::F32.byte_size(), 4);
        assert_eq!(DynamicType::I64.byte_size(), 8);
        assert_eq!(DynamicType::U64.byte_size(), 8);
        assert_eq!(DynamicType::F64.byte_size(), 8);
        assert_eq!(DynamicType::Text.byte_size(), 0);
        assert_eq!(DynamicType::Vector.byte_size(), 0);
        assert_eq!(DynamicType::Struct.byte_size(), 0);
    }

    #[test]
    fn dynamic_type_classifications() {
        assert!(DynamicType::I32.is_scalar());
        assert!(DynamicType::Bool.is_scalar());
        assert!(DynamicType::F64.is_scalar());
        assert!(!DynamicType::Text.is_scalar());
        assert!(!DynamicType::Struct.is_scalar());
        assert!(!DynamicType::Void.is_scalar());

        assert!(DynamicType::Text.is_pointer());
        assert!(DynamicType::Data.is_pointer());
        assert!(DynamicType::Vector.is_pointer());
        assert!(DynamicType::Struct.is_pointer());
        assert!(!DynamicType::I32.is_pointer());
        assert!(!DynamicType::Variant.is_pointer());
    }

    #[test]
    fn field_name_hash() {
        let fn1 = FieldName::new("score");
        let fn2 = FieldName::new("score");
        assert_eq!(fn1.hash, fn2.hash);
        assert_eq!(fn1.hash, xxh64::hash_str("score"));
        assert_eq!(fn1.tag(), fn1.hash as u8);

        let fn3 = FieldName::new("name");
        assert_ne!(fn1.hash, fn3.hash);
    }

    #[test]
    fn field_name_from_str() {
        let fn1: FieldName = "abc".into();
        assert_eq!(fn1.name, "abc");
        assert_eq!(fn1.hash, xxh64::hash_str("abc"));
    }

    #[test]
    fn field_desc_scalar() {
        let fd = FieldDesc::scalar("count", DynamicType::U32, 0);
        assert_eq!(fd.name, "count");
        assert_eq!(fd.ty, DynamicType::U32);
        assert!(!fd.is_ptr);
        assert_eq!(fd.offset, 0);
        assert_eq!(fd.byte_size, 4);
        assert_eq!(fd.name_hash, xxh64::hash_str("count"));
        assert!(fd.alternatives.is_empty());
    }

    #[test]
    fn field_desc_bool() {
        let fd = FieldDesc::bool_field("active", 2, 3);
        assert_eq!(fd.ty, DynamicType::Bool);
        assert_eq!(fd.offset, 2);
        assert_eq!(fd.bit_index, 3);
        assert_eq!(fd.byte_size, 0);
    }

    #[test]
    fn field_desc_pointer() {
        let fd = FieldDesc::pointer("label", DynamicType::Text, 0);
        assert_eq!(fd.ty, DynamicType::Text);
        assert!(fd.is_ptr);
        assert_eq!(fd.offset, 0);
    }

    #[test]
    fn schema_empty() {
        let schema = DynamicSchema::default();
        assert_eq!(schema.field_count(), 0);
        assert!(schema.find_by_name("anything").is_none());
    }

    #[test]
    fn schema_builder_basic() {
        let schema = SchemaBuilder::new()
            .field_scalar("x", DynamicType::I32, 0)
            .field_scalar("y", DynamicType::I32, 4)
            .field_pointer("name", DynamicType::Text, 0)
            .data_words(1)
            .ptr_count(1)
            .build();

        assert_eq!(schema.field_count(), 3);
        assert_eq!(schema.data_words, 1);
        assert_eq!(schema.ptr_count, 1);

        // Verify declaration order
        assert_eq!(schema.ordered_names, vec!["x", "y", "name"]);
    }

    #[test]
    fn schema_find_by_name() {
        let schema = SchemaBuilder::new()
            .field_scalar("alpha", DynamicType::U64, 0)
            .field_scalar("beta", DynamicType::I32, 8)
            .field_pointer("gamma", DynamicType::Text, 0)
            .data_words(2)
            .ptr_count(1)
            .build();

        let f = schema.find_by_name("alpha").expect("alpha not found");
        assert_eq!(f.ty, DynamicType::U64);
        assert_eq!(f.offset, 0);

        let f = schema.find_by_name("beta").expect("beta not found");
        assert_eq!(f.ty, DynamicType::I32);
        assert_eq!(f.offset, 8);

        let f = schema.find_by_name("gamma").expect("gamma not found");
        assert_eq!(f.ty, DynamicType::Text);
        assert!(f.is_ptr);

        assert!(schema.find_by_name("nonexistent").is_none());
    }

    #[test]
    fn schema_find_by_field_name() {
        let schema = SchemaBuilder::new()
            .field_scalar("score", DynamicType::F64, 0)
            .data_words(1)
            .build();

        let fn_ = FieldName::new("score");
        let f = schema.find_by_field_name(&fn_).expect("score not found");
        assert_eq!(f.ty, DynamicType::F64);
    }

    #[test]
    fn schema_find_by_hash() {
        let schema = SchemaBuilder::new()
            .field_scalar("value", DynamicType::U32, 0)
            .data_words(1)
            .build();

        let hash = xxh64::hash_str("value");
        let f = schema.find(hash, "value").expect("value not found");
        assert_eq!(f.name, "value");

        // Wrong name with same hash should not match (collision safety)
        assert!(schema.find(hash, "wrong_name").is_none());
    }

    #[test]
    fn schema_sorted_by_hash() {
        let schema = SchemaBuilder::new()
            .field_scalar("z_field", DynamicType::I32, 0)
            .field_scalar("a_field", DynamicType::I32, 4)
            .field_scalar("m_field", DynamicType::I32, 8)
            .data_words(2)
            .build();

        // sorted_fields should be sorted by name_hash
        for i in 1..schema.sorted_fields.len() {
            assert!(
                schema.sorted_fields[i - 1].name_hash
                    <= schema.sorted_fields[i].name_hash,
                "sorted_fields not sorted at index {}",
                i
            );
        }

        // tags should be parallel
        for (i, field) in schema.sorted_fields.iter().enumerate() {
            assert_eq!(schema.tags[i], field.tag());
        }

        // Declaration order should be preserved
        assert_eq!(
            schema.ordered_names,
            vec!["z_field", "a_field", "m_field"]
        );
    }

    #[test]
    fn schema_fields_ordered_iterator() {
        let schema = SchemaBuilder::new()
            .field_scalar("c", DynamicType::I32, 0)
            .field_scalar("a", DynamicType::I32, 4)
            .field_scalar("b", DynamicType::I32, 8)
            .data_words(2)
            .build();

        let names: Vec<&str> = schema.fields_ordered().map(|f| f.name.as_str()).collect();
        assert_eq!(names, vec!["c", "a", "b"]);
    }

    #[test]
    fn schema_nested_struct() {
        let inner = SchemaBuilder::new()
            .field_scalar("x", DynamicType::I32, 0)
            .field_scalar("y", DynamicType::I32, 4)
            .data_words(1)
            .build();

        let outer = SchemaBuilder::new()
            .field_struct("position", 0, inner)
            .field_scalar("id", DynamicType::U64, 0)
            .data_words(1)
            .ptr_count(1)
            .build();

        let pos = outer.find_by_name("position").expect("position not found");
        assert_eq!(pos.ty, DynamicType::Struct);
        let nested = pos.nested.as_ref().expect("missing nested schema");
        assert_eq!(nested.field_count(), 2);
        assert!(nested.find_by_name("x").is_some());
        assert!(nested.find_by_name("y").is_some());
    }

    #[test]
    fn schema_variant_field() {
        let alts = vec![
            AltDesc {
                ty: DynamicType::Void,
                ..Default::default()
            },
            AltDesc {
                ty: DynamicType::I32,
                byte_size: 4,
                offset: 4,
                ..Default::default()
            },
            AltDesc {
                ty: DynamicType::Text,
                is_ptr: true,
                offset: 0,
                ..Default::default()
            },
        ];

        let schema = SchemaBuilder::new()
            .field_variant("payload", 0, alts)
            .data_words(1)
            .ptr_count(1)
            .build();

        let f = schema.find_by_name("payload").expect("payload not found");
        assert_eq!(f.ty, DynamicType::Variant);
        assert_eq!(f.alternatives.len(), 3);
        assert_eq!(f.alternatives[0].ty, DynamicType::Void);
        assert_eq!(f.alternatives[1].ty, DynamicType::I32);
        assert_eq!(f.alternatives[2].ty, DynamicType::Text);
        assert!(f.alternatives[2].is_ptr);
    }

    #[test]
    fn dynamic_type_info_default() {
        let info = DynamicTypeInfo::default();
        assert_eq!(info.kind, DynamicType::Void);
        assert_eq!(info.active_kind, DynamicType::Void);
        assert_eq!(info.variant_index, 0);
        assert_eq!(info.byte_size, 0);
    }

    #[test]
    fn many_fields_lookup() {
        // Test with more fields to exercise the tag scanning loop
        let mut builder = SchemaBuilder::new();
        for i in 0..40u32 {
            let name = format!("field_{}", i);
            builder = builder.field_scalar(&name, DynamicType::U32, i * 4);
        }
        let schema = builder.data_words(20).build();

        assert_eq!(schema.field_count(), 40);

        // Lookup every field
        for i in 0..40u32 {
            let name = format!("field_{}", i);
            let f = schema
                .find_by_name(&name)
                .unwrap_or_else(|| panic!("{} not found", name));
            assert_eq!(f.offset, i * 4);
        }

        // Non-existent field
        assert!(schema.find_by_name("field_999").is_none());
    }
}
