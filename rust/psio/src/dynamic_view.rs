//! Dynamic dispatch: runtime field access by name over any wire format.
//!
//! Provides `DynamicView`, `DynamicListView`, and `DynamicValue` for
//! schema-driven runtime access to serialized data without static types.
//!
//! # Supported formats
//!
//! - **WIT Canonical ABI** -- fully implemented
//! - **Fracpack** -- fully implemented
//! - **Cap'n Proto / FlatBuffers** -- stubbed with `todo!()`
//!
//! # Example
//!
//! ```
//! use psio::dynamic_schema::{SchemaBuilder, DynamicType};
//! use psio::dynamic_view::{DynamicView, WireFormat};
//! use psio::wit::pack::WitPack;
//! use psio::wit::layout::compute_struct_layout;
//!
//! // Build a schema for struct { x: u32, y: u32 }
//! let (locs, _total, _align) = compute_struct_layout(&[(4, 4), (4, 4)]);
//! let schema = SchemaBuilder::new()
//!     .field_scalar("x", DynamicType::U32, locs[0].offset)
//!     .field_scalar("y", DynamicType::U32, locs[1].offset)
//!     .data_words(1)
//!     .build();
//!
//! // Pack data using WIT format
//! let buf = (42u32, 99u32).wit_pack();
//!
//! // Create a dynamic view and read fields by name
//! let view = DynamicView::new(&buf, &schema, WireFormat::Wit);
//! assert_eq!(view.field("x").as_u32(), Some(42));
//! assert_eq!(view.field("y").as_u32(), Some(99));
//! ```

use crate::dynamic_schema::{DynamicSchema, DynamicType, FieldDesc, FieldName};
use crate::xxh64;

use crate::capnp::wire as capnp_wire;
use crate::flatbuf::view as fb_view_mod;

// ---------------------------------------------------------------------------
// WireFormat
// ---------------------------------------------------------------------------

/// Wire format discriminator for dynamic dispatch.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum WireFormat {
    /// Fracpack format (variable-length header + offset table).
    Fracpack,
    /// Cap'n Proto format (data + pointer sections).
    Capnp,
    /// FlatBuffers format (vtable-based).
    Flatbuf,
    /// WIT Canonical ABI format (natural-alignment, inline layout).
    Wit,
}

// ---------------------------------------------------------------------------
// DynamicValue
// ---------------------------------------------------------------------------

/// A runtime value extracted from a dynamic view.
///
/// Borrows data from the underlying buffer where possible (Text, Data, Struct,
/// List). Scalar types are copied by value.
#[derive(Debug)]
pub enum DynamicValue<'a> {
    /// Absent or void field.
    Void,
    Bool(bool),
    I8(i8),
    I16(i16),
    I32(i32),
    I64(i64),
    U8(u8),
    U16(u16),
    U32(u32),
    U64(u64),
    F32(f32),
    F64(f64),
    /// Borrowed UTF-8 string from the underlying buffer.
    Text(&'a str),
    /// Borrowed raw bytes from the underlying buffer.
    Data(&'a [u8]),
    /// Nested struct accessed as another dynamic view.
    Struct(DynamicView<'a>),
    /// List/vector of dynamically-typed elements.
    List(DynamicListView<'a>),
}

impl<'a> DynamicValue<'a> {
    // ── Type query ─────────────────────────────────────────────────────

    /// Returns the `DynamicType` tag for this value.
    pub fn kind(&self) -> DynamicType {
        match self {
            Self::Void => DynamicType::Void,
            Self::Bool(_) => DynamicType::Bool,
            Self::I8(_) => DynamicType::I8,
            Self::I16(_) => DynamicType::I16,
            Self::I32(_) => DynamicType::I32,
            Self::I64(_) => DynamicType::I64,
            Self::U8(_) => DynamicType::U8,
            Self::U16(_) => DynamicType::U16,
            Self::U32(_) => DynamicType::U32,
            Self::U64(_) => DynamicType::U64,
            Self::F32(_) => DynamicType::F32,
            Self::F64(_) => DynamicType::F64,
            Self::Text(_) => DynamicType::Text,
            Self::Data(_) => DynamicType::Data,
            Self::Struct(_) => DynamicType::Struct,
            Self::List(_) => DynamicType::Vector,
        }
    }

    /// Returns `true` if this value is not `Void`.
    pub fn exists(&self) -> bool {
        !matches!(self, Self::Void)
    }

    // ── Exact-type accessors ───────────────────────────────────────────

    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Self::Bool(v) => Some(*v),
            _ => None,
        }
    }

    pub fn as_i8(&self) -> Option<i8> {
        match self {
            Self::I8(v) => Some(*v),
            _ => None,
        }
    }

    pub fn as_i16(&self) -> Option<i16> {
        match self {
            Self::I16(v) => Some(*v),
            _ => None,
        }
    }

    pub fn as_i32(&self) -> Option<i32> {
        match self {
            Self::I32(v) => Some(*v),
            _ => None,
        }
    }

    pub fn as_i64(&self) -> Option<i64> {
        match self {
            Self::I64(v) => Some(*v),
            _ => None,
        }
    }

    pub fn as_u8(&self) -> Option<u8> {
        match self {
            Self::U8(v) => Some(*v),
            _ => None,
        }
    }

    pub fn as_u16(&self) -> Option<u16> {
        match self {
            Self::U16(v) => Some(*v),
            _ => None,
        }
    }

    pub fn as_u32(&self) -> Option<u32> {
        match self {
            Self::U32(v) => Some(*v),
            _ => None,
        }
    }

    pub fn as_u64(&self) -> Option<u64> {
        match self {
            Self::U64(v) => Some(*v),
            _ => None,
        }
    }

    pub fn as_f32(&self) -> Option<f32> {
        match self {
            Self::F32(v) => Some(*v),
            _ => None,
        }
    }

    pub fn as_f64(&self) -> Option<f64> {
        match self {
            Self::F64(v) => Some(*v),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&'a str> {
        match self {
            Self::Text(v) => Some(v),
            _ => None,
        }
    }

    pub fn as_bytes(&self) -> Option<&'a [u8]> {
        match self {
            Self::Data(v) => Some(v),
            _ => None,
        }
    }

    // ── Widening conversions ───────────────────────────────────────────

    /// Widen any integer type to `u64`. Returns `None` for non-integer types.
    pub fn to_u64(&self) -> Option<u64> {
        match self {
            Self::Bool(v) => Some(*v as u64),
            Self::U8(v) => Some(*v as u64),
            Self::U16(v) => Some(*v as u64),
            Self::U32(v) => Some(*v as u64),
            Self::U64(v) => Some(*v),
            Self::I8(v) => Some(*v as u64),
            Self::I16(v) => Some(*v as u64),
            Self::I32(v) => Some(*v as u64),
            Self::I64(v) => Some(*v as u64),
            _ => None,
        }
    }

    /// Widen any numeric type to `f64`. Returns `None` for non-numeric types.
    pub fn to_f64(&self) -> Option<f64> {
        match self {
            Self::Bool(v) => Some(if *v { 1.0 } else { 0.0 }),
            Self::U8(v) => Some(*v as f64),
            Self::U16(v) => Some(*v as f64),
            Self::U32(v) => Some(*v as f64),
            Self::U64(v) => Some(*v as f64),
            Self::I8(v) => Some(*v as f64),
            Self::I16(v) => Some(*v as f64),
            Self::I32(v) => Some(*v as f64),
            Self::I64(v) => Some(*v as f64),
            Self::F32(v) => Some(*v as f64),
            Self::F64(v) => Some(*v),
            _ => None,
        }
    }

    // ── Chained field access ───────────────────────────────────────────

    /// Access a sub-field when this value is a `Struct`. Returns `Void` otherwise.
    pub fn field(&self, name: &str) -> DynamicValue<'a> {
        match self {
            Self::Struct(view) => view.field(name),
            _ => DynamicValue::Void,
        }
    }

    /// Index into this value when it is a `List`. Returns `Void` otherwise.
    pub fn get(&self, index: usize) -> DynamicValue<'a> {
        match self {
            Self::List(list) => list.get(index),
            _ => DynamicValue::Void,
        }
    }

    // ── Path navigation ────────────────────────────────────────────────

    /// Navigate a dotted path like `"a.b.c[1].d"` through nested structs and
    /// lists. Returns `Void` if any step fails.
    pub fn path(&self, path: &str) -> DynamicValue<'a> {
        navigate_path_on_value(self, path)
    }
}

// ---------------------------------------------------------------------------
// DynamicValue — PartialEq and PartialOrd
// ---------------------------------------------------------------------------

/// Widen a DynamicValue to a canonical numeric representation for comparison.
///
/// Returns `(is_signed, unsigned_bits, signed_value, is_float, f64_value)`.
/// For integers: is_float=false, and we carry both the u64 reinterpretation
/// and the signed i128 for cross-sign comparison.
/// For floats: is_float=true, and f64_value holds the value.
enum NumericRepr {
    /// Unsigned integer with value.
    Unsigned(u64),
    /// Signed integer with value (may be negative).
    Signed(i64),
    /// Floating-point value.
    Float(f64),
}

impl NumericRepr {
    fn from_value(val: &DynamicValue<'_>) -> Option<Self> {
        match val {
            DynamicValue::Bool(v) => Some(NumericRepr::Unsigned(*v as u64)),
            DynamicValue::U8(v) => Some(NumericRepr::Unsigned(*v as u64)),
            DynamicValue::U16(v) => Some(NumericRepr::Unsigned(*v as u64)),
            DynamicValue::U32(v) => Some(NumericRepr::Unsigned(*v as u64)),
            DynamicValue::U64(v) => Some(NumericRepr::Unsigned(*v)),
            DynamicValue::I8(v) => Some(NumericRepr::Signed(*v as i64)),
            DynamicValue::I16(v) => Some(NumericRepr::Signed(*v as i64)),
            DynamicValue::I32(v) => Some(NumericRepr::Signed(*v as i64)),
            DynamicValue::I64(v) => Some(NumericRepr::Signed(*v)),
            DynamicValue::F32(v) => Some(NumericRepr::Float(*v as f64)),
            DynamicValue::F64(v) => Some(NumericRepr::Float(*v)),
            _ => None,
        }
    }
}

fn numeric_eq(a: &NumericRepr, b: &NumericRepr) -> bool {
    match (a, b) {
        (NumericRepr::Unsigned(x), NumericRepr::Unsigned(y)) => x == y,
        (NumericRepr::Signed(x), NumericRepr::Signed(y)) => x == y,
        (NumericRepr::Unsigned(x), NumericRepr::Signed(y)) => {
            // Unsigned == Signed: only if signed >= 0 and values match
            *y >= 0 && *x == *y as u64
        }
        (NumericRepr::Signed(x), NumericRepr::Unsigned(y)) => {
            *x >= 0 && *x as u64 == *y
        }
        (NumericRepr::Float(x), NumericRepr::Float(y)) => x == y,
        (NumericRepr::Float(x), NumericRepr::Unsigned(y)) => *x == *y as f64,
        (NumericRepr::Float(x), NumericRepr::Signed(y)) => *x == *y as f64,
        (NumericRepr::Unsigned(x), NumericRepr::Float(y)) => *x as f64 == *y,
        (NumericRepr::Signed(x), NumericRepr::Float(y)) => *x as f64 == *y,
    }
}

fn numeric_partial_cmp(a: &NumericRepr, b: &NumericRepr) -> Option<std::cmp::Ordering> {
    match (a, b) {
        (NumericRepr::Unsigned(x), NumericRepr::Unsigned(y)) => x.partial_cmp(y),
        (NumericRepr::Signed(x), NumericRepr::Signed(y)) => x.partial_cmp(y),
        (NumericRepr::Unsigned(x), NumericRepr::Signed(y)) => {
            if *y < 0 {
                // any unsigned is > negative signed
                Some(std::cmp::Ordering::Greater)
            } else {
                (*x).partial_cmp(&(*y as u64))
            }
        }
        (NumericRepr::Signed(x), NumericRepr::Unsigned(y)) => {
            if *x < 0 {
                Some(std::cmp::Ordering::Less)
            } else {
                (*x as u64).partial_cmp(y)
            }
        }
        (NumericRepr::Float(x), NumericRepr::Float(y)) => x.partial_cmp(y),
        (NumericRepr::Float(x), NumericRepr::Unsigned(y)) => x.partial_cmp(&(*y as f64)),
        (NumericRepr::Float(x), NumericRepr::Signed(y)) => x.partial_cmp(&(*y as f64)),
        (NumericRepr::Unsigned(x), NumericRepr::Float(y)) => (*x as f64).partial_cmp(y),
        (NumericRepr::Signed(x), NumericRepr::Float(y)) => (*x as f64).partial_cmp(y),
    }
}

impl<'a> PartialEq for DynamicValue<'a> {
    fn eq(&self, other: &Self) -> bool {
        // Same-variant fast path
        match (self, other) {
            (DynamicValue::Void, DynamicValue::Void) => return true,
            (DynamicValue::Text(a), DynamicValue::Text(b)) => return a == b,
            (DynamicValue::Data(a), DynamicValue::Data(b)) => return a == b,
            _ => {}
        }

        // Try numeric cross-type comparison
        if let (Some(a), Some(b)) = (NumericRepr::from_value(self), NumericRepr::from_value(other)) {
            return numeric_eq(&a, &b);
        }

        false
    }
}

impl<'a> PartialOrd for DynamicValue<'a> {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        // Same-variant fast paths
        match (self, other) {
            (DynamicValue::Void, DynamicValue::Void) => return Some(std::cmp::Ordering::Equal),
            (DynamicValue::Text(a), DynamicValue::Text(b)) => return a.partial_cmp(b),
            (DynamicValue::Data(a), DynamicValue::Data(b)) => return a.partial_cmp(b),
            _ => {}
        }

        // Try numeric cross-type comparison
        if let (Some(a), Some(b)) = (NumericRepr::from_value(self), NumericRepr::from_value(other)) {
            return numeric_partial_cmp(&a, &b);
        }

        None
    }
}

// ---------------------------------------------------------------------------
// DynamicView
// ---------------------------------------------------------------------------

/// Zero-copy runtime view over serialized struct data.
///
/// Uses a `DynamicSchema` to locate fields by name within the raw bytes,
/// dispatching reads to the appropriate wire-format reader.
#[derive(Debug, Clone)]
pub struct DynamicView<'a> {
    data: &'a [u8],
    schema: &'a DynamicSchema,
    format: WireFormat,
    /// Byte offset within `data` where this record begins (for nested structs).
    rec_offset: u32,
}

impl<'a> DynamicView<'a> {
    /// Create a view over the root of a buffer.
    pub fn new(data: &'a [u8], schema: &'a DynamicSchema, format: WireFormat) -> Self {
        Self {
            data,
            schema,
            format,
            rec_offset: 0,
        }
    }

    /// Create a view at a specific byte offset within the buffer.
    pub fn at_offset(
        data: &'a [u8],
        schema: &'a DynamicSchema,
        format: WireFormat,
        offset: u32,
    ) -> Self {
        Self {
            data,
            schema,
            format,
            rec_offset: offset,
        }
    }

    /// The wire format this view interprets.
    pub fn format(&self) -> WireFormat {
        self.format
    }

    /// The schema driving this view.
    pub fn schema(&self) -> &'a DynamicSchema {
        self.schema
    }

    /// The type of this view is always `Struct`.
    pub fn kind(&self) -> DynamicType {
        DynamicType::Struct
    }

    /// Number of fields in the schema.
    pub fn field_count(&self) -> usize {
        self.schema.field_count()
    }

    /// Iterate field names in declaration order.
    pub fn field_names(&self) -> impl Iterator<Item = &str> {
        self.schema.ordered_names.iter().map(|s| s.as_str())
    }

    /// Look up a field by name. Returns `DynamicValue::Void` if not found.
    pub fn field(&self, name: &str) -> DynamicValue<'a> {
        let hash = xxh64::hash_str(name);
        self.field_by_hash(hash, name)
    }

    /// Look up a field with a pre-computed hash (avoids re-hashing).
    pub fn field_by_hash(&self, hash: u64, name: &str) -> DynamicValue<'a> {
        match self.schema.find(hash, name) {
            Some(desc) => self.read_field(desc),
            None => DynamicValue::Void,
        }
    }

    /// Look up a field using a `FieldName` (has pre-computed hash).
    pub fn field_by_field_name(&self, name: &FieldName) -> DynamicValue<'a> {
        match self.schema.find_by_field_name(name) {
            Some(desc) => self.read_field(desc),
            None => DynamicValue::Void,
        }
    }

    /// Navigate a dotted path like `"a.b[0].c"` from this view.
    pub fn path(&self, path_str: &str) -> DynamicValue<'a> {
        let val = DynamicValue::Struct(self.clone());
        navigate_path_on_value(&val, path_str)
    }

    // ── Format-specific field reading ──────────────────────────────────

    fn read_field(&self, desc: &'a FieldDesc) -> DynamicValue<'a> {
        match self.format {
            WireFormat::Wit => self.read_field_wit(desc),
            WireFormat::Fracpack => self.read_field_fracpack(desc),
            WireFormat::Capnp => self.read_field_capnp(desc),
            WireFormat::Flatbuf => self.read_field_flatbuf(desc),
        }
    }

    // ── WIT Canonical ABI reader ───────────────────────────────────────

    /// Read a field from WIT Canonical ABI encoded bytes.
    ///
    /// In WIT, all scalars and sub-records are stored inline at naturally-aligned
    /// offsets. Strings and lists are stored as `(ptr: u32, len: u32)` with
    /// out-of-line data.
    fn read_field_wit(&self, desc: &'a FieldDesc) -> DynamicValue<'a> {
        let base = self.rec_offset + desc.offset;

        match desc.ty {
            DynamicType::Bool => {
                let v = self.read_u8_at(base);
                DynamicValue::Bool(v != 0)
            }
            DynamicType::I8 => DynamicValue::I8(self.read_u8_at(base) as i8),
            DynamicType::U8 => DynamicValue::U8(self.read_u8_at(base)),
            DynamicType::I16 => DynamicValue::I16(self.read_le_u16(base) as i16),
            DynamicType::U16 => DynamicValue::U16(self.read_le_u16(base)),
            DynamicType::I32 => DynamicValue::I32(self.read_le_u32(base) as i32),
            DynamicType::U32 => DynamicValue::U32(self.read_le_u32(base)),
            DynamicType::I64 => DynamicValue::I64(self.read_le_u64(base) as i64),
            DynamicType::U64 => DynamicValue::U64(self.read_le_u64(base)),
            DynamicType::F32 => {
                DynamicValue::F32(f32::from_le_bytes(self.read_bytes_4(base)))
            }
            DynamicType::F64 => {
                DynamicValue::F64(f64::from_le_bytes(self.read_bytes_8(base)))
            }
            DynamicType::Text => {
                // WIT stores strings as (ptr: u32, len: u32)
                let ptr = self.read_le_u32(base) as usize;
                let len = self.read_le_u32(base + 4) as usize;
                if ptr + len > self.data.len() {
                    return DynamicValue::Void;
                }
                match std::str::from_utf8(&self.data[ptr..ptr + len]) {
                    Ok(s) => DynamicValue::Text(s),
                    Err(_) => DynamicValue::Void,
                }
            }
            DynamicType::Data => {
                // Same encoding as text but returns raw bytes
                let ptr = self.read_le_u32(base) as usize;
                let len = self.read_le_u32(base + 4) as usize;
                if ptr + len > self.data.len() {
                    return DynamicValue::Void;
                }
                DynamicValue::Data(&self.data[ptr..ptr + len])
            }
            DynamicType::Struct => {
                // In WIT, nested structs are inline at the field offset
                if let Some(ref nested) = desc.nested {
                    DynamicValue::Struct(DynamicView {
                        data: self.data,
                        schema: nested,
                        format: WireFormat::Wit,
                        rec_offset: base,
                    })
                } else {
                    DynamicValue::Void
                }
            }
            DynamicType::Vector => {
                // WIT stores lists as (ptr: u32, count: u32)
                let ptr = self.read_le_u32(base);
                let count = self.read_le_u32(base + 4);
                DynamicValue::List(DynamicListView {
                    data: self.data,
                    format: WireFormat::Wit,
                    arr_offset: ptr,
                    count,
                    elem_desc: desc,
                })
            }
            DynamicType::Variant | DynamicType::Void => DynamicValue::Void,
        }
    }

    // ── Fracpack reader ────────────────────────────────────────────────

    /// Read a field from fracpack-encoded bytes.
    ///
    /// Fracpack uses a fixed-size header followed by a variable-length offset
    /// table. `FieldDesc.offset` holds the byte offset within the fixed header
    /// for inline scalars, or a pointer-table index for variable-length fields.
    ///
    /// For the dynamic view, the schema's `FieldDesc` is set up with:
    /// - `is_ptr == false, offset == byte_offset` for inline scalars
    /// - `is_ptr == true, offset == byte_offset_of_u32_pointer` for strings/data/structs/vectors
    ///
    /// Fracpack encodes variable-length data with a 4-byte relative offset at the
    /// pointer location. The actual data starts at `pointer_location + offset_value`.
    fn read_field_fracpack(&self, desc: &'a FieldDesc) -> DynamicValue<'a> {
        let base = self.rec_offset + desc.offset;

        if !desc.is_ptr {
            // Inline scalar field: read directly from the fixed header
            match desc.ty {
                DynamicType::Bool => {
                    let byte = self.read_u8_at(base);
                    DynamicValue::Bool((byte >> desc.bit_index) & 1 != 0)
                }
                DynamicType::I8 => DynamicValue::I8(self.read_u8_at(base) as i8),
                DynamicType::U8 => DynamicValue::U8(self.read_u8_at(base)),
                DynamicType::I16 => DynamicValue::I16(self.read_le_u16(base) as i16),
                DynamicType::U16 => DynamicValue::U16(self.read_le_u16(base)),
                DynamicType::I32 => DynamicValue::I32(self.read_le_u32(base) as i32),
                DynamicType::U32 => DynamicValue::U32(self.read_le_u32(base)),
                DynamicType::I64 => DynamicValue::I64(self.read_le_u64(base) as i64),
                DynamicType::U64 => DynamicValue::U64(self.read_le_u64(base)),
                DynamicType::F32 => {
                    DynamicValue::F32(f32::from_le_bytes(self.read_bytes_4(base)))
                }
                DynamicType::F64 => {
                    DynamicValue::F64(f64::from_le_bytes(self.read_bytes_8(base)))
                }
                _ => DynamicValue::Void,
            }
        } else {
            // Pointer field: read the offset, then read the pointed-to data.
            // Fracpack stores a u32 relative offset. If the offset is 0 the
            // field is absent/empty.
            let rel_offset = self.read_le_u32(base);
            if rel_offset == 0 {
                return DynamicValue::Void;
            }
            let abs = base + rel_offset;

            match desc.ty {
                DynamicType::Text => {
                    // Fracpack strings: the offset points to a u32 length prefix
                    // followed by the string data.
                    if abs as usize + 4 > self.data.len() {
                        return DynamicValue::Void;
                    }
                    let len = self.read_le_u32(abs) as usize;
                    let str_start = abs as usize + 4;
                    if str_start + len > self.data.len() {
                        return DynamicValue::Void;
                    }
                    match std::str::from_utf8(&self.data[str_start..str_start + len]) {
                        Ok(s) => DynamicValue::Text(s),
                        Err(_) => DynamicValue::Void,
                    }
                }
                DynamicType::Data => {
                    if abs as usize + 4 > self.data.len() {
                        return DynamicValue::Void;
                    }
                    let len = self.read_le_u32(abs) as usize;
                    let start = abs as usize + 4;
                    if start + len > self.data.len() {
                        return DynamicValue::Void;
                    }
                    DynamicValue::Data(&self.data[start..start + len])
                }
                DynamicType::Struct => {
                    if let Some(ref nested) = desc.nested {
                        // Fracpack structs: the offset points to a u16 fixed_size
                        // header, followed by the fixed data. For dynamic views we
                        // treat `abs` as the start of the nested record (after the
                        // caller has set up the schema offsets relative to that).
                        DynamicValue::Struct(DynamicView {
                            data: self.data,
                            schema: nested,
                            format: WireFormat::Fracpack,
                            rec_offset: abs,
                        })
                    } else {
                        DynamicValue::Void
                    }
                }
                DynamicType::Vector => {
                    // Fracpack vectors: offset points to a u32 length prefix
                    // (count of elements), then the element data.
                    if abs as usize + 4 > self.data.len() {
                        return DynamicValue::Void;
                    }
                    let count = self.read_le_u32(abs);
                    let arr_start = abs + 4;
                    DynamicValue::List(DynamicListView {
                        data: self.data,
                        format: WireFormat::Fracpack,
                        arr_offset: arr_start,
                        count,
                        elem_desc: desc,
                    })
                }
                _ => DynamicValue::Void,
            }
        }
    }

    // ── Cap'n Proto reader ──────────────────────────────────────────────

    /// Read a field from a Cap'n Proto message.
    ///
    /// The DynamicView for capnp stores the full message in `data`.
    /// `rec_offset == 0` means root; otherwise it is the data_offset within
    /// the segment (relative to the segment start, i.e., after the 8-byte
    /// segment table).
    ///
    /// FieldDesc layout for capnp:
    /// - `is_ptr == false`: scalar field; `offset` is the byte offset within the
    ///   data section, `bit_index` for bools.
    /// - `is_ptr == true`: pointer field; `offset` is the pointer slot index.
    ///
    /// The schema's `data_words` and `ptr_count` describe the struct size.
    fn read_field_capnp(&self, desc: &'a FieldDesc) -> DynamicValue<'a> {
        // Parse the segment table. Segment starts at byte 8.
        let msg = self.data;
        let seg_start = capnp_segment_start(msg);
        if seg_start == 0 {
            return DynamicValue::Void;
        }
        let segment = &msg[seg_start..];

        let ptr = if self.rec_offset == 0 {
            // Root: resolve from the root pointer word
            match capnp_wire::resolve_struct_ptr(segment, 0) {
                Some(p) => p,
                None => return DynamicValue::Void,
            }
        } else {
            capnp_wire::CapnpPtr {
                data_offset: self.rec_offset as usize,
                data_words: self.schema.data_words,
                ptr_count: self.schema.ptr_count,
            }
        };

        if !desc.is_ptr {
            let off = ptr.data_offset + desc.offset as usize;
            match desc.ty {
                DynamicType::Bool => {
                    if off >= segment.len() {
                        return DynamicValue::Bool(false);
                    }
                    DynamicValue::Bool((segment[off] >> desc.bit_index) & 1 != 0)
                }
                DynamicType::U8 => {
                    if off >= segment.len() { return DynamicValue::U8(0); }
                    DynamicValue::U8(segment[off])
                }
                DynamicType::I8 => {
                    if off >= segment.len() { return DynamicValue::I8(0); }
                    DynamicValue::I8(segment[off] as i8)
                }
                DynamicType::U16 => {
                    if off + 2 > segment.len() { return DynamicValue::U16(0); }
                    DynamicValue::U16(capnp_wire::read_u16(segment, off))
                }
                DynamicType::I16 => {
                    if off + 2 > segment.len() { return DynamicValue::I16(0); }
                    DynamicValue::I16(capnp_wire::read_u16(segment, off) as i16)
                }
                DynamicType::U32 => {
                    if off + 4 > segment.len() { return DynamicValue::U32(0); }
                    DynamicValue::U32(capnp_wire::read_u32(segment, off))
                }
                DynamicType::I32 => {
                    if off + 4 > segment.len() { return DynamicValue::I32(0); }
                    DynamicValue::I32(capnp_wire::read_u32(segment, off) as i32)
                }
                DynamicType::U64 => {
                    if off + 8 > segment.len() { return DynamicValue::U64(0); }
                    DynamicValue::U64(capnp_wire::read_u64(segment, off))
                }
                DynamicType::I64 => {
                    if off + 8 > segment.len() { return DynamicValue::I64(0); }
                    DynamicValue::I64(capnp_wire::read_u64(segment, off) as i64)
                }
                DynamicType::F32 => {
                    if off + 4 > segment.len() { return DynamicValue::F32(0.0); }
                    DynamicValue::F32(f32::from_bits(capnp_wire::read_u32(segment, off)))
                }
                DynamicType::F64 => {
                    if off + 8 > segment.len() { return DynamicValue::F64(0.0); }
                    DynamicValue::F64(f64::from_bits(capnp_wire::read_u64(segment, off)))
                }
                _ => DynamicValue::Void,
            }
        } else {
            let slot_offset = ptr.ptr_slot_offset(desc.offset);
            match desc.ty {
                DynamicType::Text => {
                    DynamicValue::Text(capnp_wire::read_text(segment, slot_offset))
                }
                DynamicType::Struct => {
                    if let Some(ref nested) = desc.nested {
                        match capnp_wire::resolve_struct_ptr(segment, slot_offset) {
                            Some(child) => {
                                DynamicValue::Struct(DynamicView {
                                    data: self.data,
                                    schema: nested,
                                    format: WireFormat::Capnp,
                                    rec_offset: child.data_offset as u32,
                                })
                            }
                            None => DynamicValue::Void,
                        }
                    } else {
                        DynamicValue::Void
                    }
                }
                DynamicType::Vector => {
                    match capnp_wire::resolve_list_ptr(segment, slot_offset) {
                        Some(info) => {
                            // arr_offset must be absolute within self.data, not
                            // relative to the segment.
                            DynamicValue::List(DynamicListView {
                                data: self.data,
                                format: WireFormat::Capnp,
                                arr_offset: (seg_start + info.data_offset) as u32,
                                count: info.count,
                                elem_desc: desc,
                            })
                        }
                        None => DynamicValue::Void,
                    }
                }
                DynamicType::Data => {
                    match capnp_wire::resolve_list_ptr(segment, slot_offset) {
                        Some(info) => {
                            let abs_start = seg_start + info.data_offset;
                            let end = abs_start + info.count as usize;
                            if end <= msg.len() {
                                DynamicValue::Data(&msg[abs_start..end])
                            } else {
                                DynamicValue::Void
                            }
                        }
                        None => DynamicValue::Void,
                    }
                }
                _ => DynamicValue::Void,
            }
        }
    }

    // ── FlatBuffers reader ────────────────────────────────────────────────

    /// Read a field from FlatBuffer-encoded bytes.
    ///
    /// The DynamicView for flatbuf is created with `rec_offset == 0` for the root
    /// and `rec_offset == table_pos` for nested tables.
    ///
    /// FieldDesc layout for flatbuf:
    /// - `offset` is the vtable slot index (0-based).
    /// - `is_ptr == true` for text, struct, vector, data fields.
    /// - `is_ptr == false` for scalar fields.
    fn read_field_flatbuf(&self, desc: &'a FieldDesc) -> DynamicValue<'a> {
        let buf = self.data;

        // Determine the table position.
        let table_pos = if self.rec_offset == 0 {
            // Root: read root offset from first 4 bytes
            if buf.len() < 4 {
                return DynamicValue::Void;
            }
            let root_off = u32::from_le_bytes(buf[0..4].try_into().unwrap()) as usize;
            root_off
        } else {
            self.rec_offset as usize
        };

        let fb = match fb_view_mod::FbView::from_table(buf, table_pos) {
            Ok(v) => v,
            Err(_) => return DynamicValue::Void,
        };

        let slot = desc.offset as u16;

        if !desc.is_ptr {
            // Scalar field
            match desc.ty {
                DynamicType::Bool => DynamicValue::Bool(fb.read_bool(slot, false)),
                DynamicType::U8 => DynamicValue::U8(fb.read_u8(slot, 0)),
                DynamicType::I8 => DynamicValue::I8(fb.read_i8(slot, 0)),
                DynamicType::U16 => DynamicValue::U16(fb.read_u16(slot, 0)),
                DynamicType::I16 => DynamicValue::I16(fb.read_i16(slot, 0)),
                DynamicType::U32 => DynamicValue::U32(fb.read_u32(slot, 0)),
                DynamicType::I32 => DynamicValue::I32(fb.read_i32(slot, 0)),
                DynamicType::U64 => DynamicValue::U64(fb.read_u64(slot, 0)),
                DynamicType::I64 => DynamicValue::I64(fb.read_i64(slot, 0)),
                DynamicType::F32 => DynamicValue::F32(fb.read_f32(slot, 0.0)),
                DynamicType::F64 => DynamicValue::F64(fb.read_f64(slot, 0.0)),
                _ => DynamicValue::Void,
            }
        } else {
            // Pointer field
            match desc.ty {
                DynamicType::Text => {
                    match fb.read_str(slot) {
                        Some(s) => DynamicValue::Text(s),
                        None => DynamicValue::Text(""),
                    }
                }
                DynamicType::Struct => {
                    if let Some(ref nested) = desc.nested {
                        match fb_resolve_sub_table(buf, table_pos, slot) {
                            Some(sub_pos) => {
                                DynamicValue::Struct(DynamicView {
                                    data: self.data,
                                    schema: nested,
                                    format: WireFormat::Flatbuf,
                                    rec_offset: sub_pos as u32,
                                })
                            }
                            None => DynamicValue::Void,
                        }
                    } else {
                        DynamicValue::Void
                    }
                }
                DynamicType::Vector => {
                    match fb_resolve_vec(buf, table_pos, slot) {
                        Some((data_start, count)) => {
                            DynamicValue::List(DynamicListView {
                                data: self.data,
                                format: WireFormat::Flatbuf,
                                arr_offset: data_start as u32,
                                count,
                                elem_desc: desc,
                            })
                        }
                        None => DynamicValue::Void,
                    }
                }
                DynamicType::Data => {
                    // Data as a vector of bytes
                    match fb_resolve_vec(buf, table_pos, slot) {
                        Some((data_start, count)) => {
                            let end = data_start + count as usize;
                            if end <= buf.len() {
                                DynamicValue::Data(&buf[data_start..end])
                            } else {
                                DynamicValue::Void
                            }
                        }
                        None => DynamicValue::Void,
                    }
                }
                _ => DynamicValue::Void,
            }
        }
    }

    // ── Raw byte reading helpers ───────────────────────────────────────

    fn read_u8_at(&self, off: u32) -> u8 {
        self.data[off as usize]
    }

    fn read_le_u16(&self, off: u32) -> u16 {
        let s = off as usize;
        u16::from_le_bytes(self.data[s..s + 2].try_into().unwrap())
    }

    fn read_le_u32(&self, off: u32) -> u32 {
        let s = off as usize;
        u32::from_le_bytes(self.data[s..s + 4].try_into().unwrap())
    }

    fn read_le_u64(&self, off: u32) -> u64 {
        let s = off as usize;
        u64::from_le_bytes(self.data[s..s + 8].try_into().unwrap())
    }

    fn read_bytes_4(&self, off: u32) -> [u8; 4] {
        let s = off as usize;
        self.data[s..s + 4].try_into().unwrap()
    }

    fn read_bytes_8(&self, off: u32) -> [u8; 8] {
        let s = off as usize;
        self.data[s..s + 8].try_into().unwrap()
    }
}

// ---------------------------------------------------------------------------
// Cap'n Proto helpers
// ---------------------------------------------------------------------------

/// Return the byte offset where the segment data starts in a capnp message.
/// Returns 0 on error. For single-segment messages this is always 8.
fn capnp_segment_start(msg: &[u8]) -> usize {
    match capnp_wire::parse_segment_table(msg) {
        Ok((start, _)) => start,
        Err(_) => 0,
    }
}

// ---------------------------------------------------------------------------
// FlatBuffers helpers
// ---------------------------------------------------------------------------

/// Resolve a flatbuf sub-table position from a parent table and slot.
/// `parent_table_pos` is the parent table's position in the buffer.
/// Returns the sub-table position.
fn fb_resolve_sub_table(buf: &[u8], parent_table_pos: usize, slot: u16) -> Option<usize> {
    if parent_table_pos + 4 > buf.len() {
        return None;
    }
    // Read vtable soffset
    let soff = i32::from_le_bytes(buf[parent_table_pos..parent_table_pos + 4].try_into().ok()?);
    let vt_pos = (parent_table_pos as i64 - soff as i64) as usize;
    if vt_pos + 4 > buf.len() {
        return None;
    }
    let vt_size = u16::from_le_bytes(buf[vt_pos..vt_pos + 2].try_into().ok()?);
    let vt_byte = 4 + 2 * slot;
    if vt_byte + 2 > vt_size {
        return None;
    }
    let fo = u16::from_le_bytes(
        buf[vt_pos + vt_byte as usize..vt_pos + vt_byte as usize + 2]
            .try_into()
            .ok()?,
    );
    if fo == 0 {
        return None;
    }
    let ref_pos = parent_table_pos + fo as usize;
    if ref_pos + 4 > buf.len() {
        return None;
    }
    let rel = u32::from_le_bytes(buf[ref_pos..ref_pos + 4].try_into().ok()?);
    Some(ref_pos + rel as usize)
}

/// Resolve a flatbuf vector data position from a parent table and slot.
/// Returns (data_start, count).
fn fb_resolve_vec(buf: &[u8], parent_table_pos: usize, slot: u16) -> Option<(usize, u32)> {
    if parent_table_pos + 4 > buf.len() {
        return None;
    }
    let soff = i32::from_le_bytes(buf[parent_table_pos..parent_table_pos + 4].try_into().ok()?);
    let vt_pos = (parent_table_pos as i64 - soff as i64) as usize;
    if vt_pos + 4 > buf.len() {
        return None;
    }
    let vt_size = u16::from_le_bytes(buf[vt_pos..vt_pos + 2].try_into().ok()?);
    let vt_byte = 4 + 2 * slot;
    if vt_byte + 2 > vt_size {
        return None;
    }
    let fo = u16::from_le_bytes(
        buf[vt_pos + vt_byte as usize..vt_pos + vt_byte as usize + 2]
            .try_into()
            .ok()?,
    );
    if fo == 0 {
        return None;
    }
    let ref_pos = parent_table_pos + fo as usize;
    if ref_pos + 4 > buf.len() {
        return None;
    }
    let rel = u32::from_le_bytes(buf[ref_pos..ref_pos + 4].try_into().ok()?);
    let vec_pos = ref_pos + rel as usize;
    if vec_pos + 4 > buf.len() {
        return None;
    }
    let count = u32::from_le_bytes(buf[vec_pos..vec_pos + 4].try_into().ok()?);
    Some((vec_pos + 4, count))
}

// ---------------------------------------------------------------------------
// DynamicListView
// ---------------------------------------------------------------------------

/// Zero-copy view over a dynamically-typed list/vector.
///
/// Element access returns `DynamicValue` for each element based on the
/// element type from the parent field's schema.
#[derive(Debug, Clone)]
pub struct DynamicListView<'a> {
    data: &'a [u8],
    format: WireFormat,
    arr_offset: u32,
    count: u32,
    /// The field descriptor of the parent vector field. The element type is
    /// inferred from `elem_desc.nested` (for struct elements) or the scalar
    /// type stored in a vector element descriptor.
    elem_desc: &'a FieldDesc,
}

impl<'a> DynamicListView<'a> {
    /// Number of elements.
    pub fn len(&self) -> usize {
        self.count as usize
    }

    /// Whether the list is empty.
    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// Access element at `index`. Returns `Void` if out of bounds.
    pub fn get(&self, index: usize) -> DynamicValue<'a> {
        if index >= self.count as usize {
            return DynamicValue::Void;
        }

        // Determine element type from the field descriptor.
        // For vector fields, `elem_desc.nested` holds the element schema
        // (for struct elements) and `elem_desc.byte_size` holds the element
        // size for scalar vectors.
        let elem_nested = self.elem_desc.nested.as_deref();

        // If there is a nested schema, elements are structs
        if let Some(nested) = elem_nested {
            // Calculate element size from the nested schema.
            // For WIT, struct size comes from layout. We use the byte_size
            // stored in the FieldDesc if > 0, otherwise compute from schema.
            let elem_size = if self.elem_desc.byte_size > 0 {
                self.elem_desc.byte_size as u32
            } else {
                // Fallback: use data_words * 8 for capnp-style, but for WIT
                // we need the actual packed struct size. Use byte_size from
                // the FieldDesc which should be set by the schema builder.
                // If not set, we cannot iterate safely.
                return DynamicValue::Void;
            };
            let offset = self.arr_offset + index as u32 * elem_size;
            DynamicValue::Struct(DynamicView {
                data: self.data,
                schema: nested,
                format: self.format,
                rec_offset: offset,
            })
        } else {
            // Scalar or string element
            let elem_size = self.elem_desc.byte_size as u32;
            if elem_size == 0 {
                return DynamicValue::Void;
            }
            let offset = self.arr_offset + index as u32 * elem_size;
            self.read_scalar_at(offset)
        }
    }

    /// Iterate over all elements.
    pub fn iter(&self) -> DynamicListIter<'a> {
        DynamicListIter {
            list: self.clone(),
            index: 0,
        }
    }

    fn read_scalar_at(&self, off: u32) -> DynamicValue<'a> {
        // For scalar vectors we need to know the element type. We use
        // a heuristic based on byte_size from the parent FieldDesc.
        // The FieldDesc for a vector doesn't directly store element type,
        // so we use the byte_size field which the schema builder should set
        // to the element size.
        let s = off as usize;
        match self.elem_desc.byte_size {
            1 => DynamicValue::U8(self.data[s]),
            2 => {
                let v = u16::from_le_bytes(self.data[s..s + 2].try_into().unwrap());
                DynamicValue::U16(v)
            }
            4 => {
                let v = u32::from_le_bytes(self.data[s..s + 4].try_into().unwrap());
                DynamicValue::U32(v)
            }
            8 => {
                let v = u64::from_le_bytes(self.data[s..s + 8].try_into().unwrap());
                DynamicValue::U64(v)
            }
            _ => DynamicValue::Void,
        }
    }
}

/// Iterator over `DynamicListView` elements.
pub struct DynamicListIter<'a> {
    list: DynamicListView<'a>,
    index: usize,
}

impl<'a> Iterator for DynamicListIter<'a> {
    type Item = DynamicValue<'a>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.index < self.list.len() {
            let val = self.list.get(self.index);
            self.index += 1;
            Some(val)
        } else {
            None
        }
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        let remaining = self.list.len() - self.index;
        (remaining, Some(remaining))
    }
}

impl<'a> ExactSizeIterator for DynamicListIter<'a> {}

// ---------------------------------------------------------------------------
// Path navigation
// ---------------------------------------------------------------------------

/// Parse and navigate a dotted path with optional array indexing.
///
/// Supports paths like:
/// - `"name"` -- single field
/// - `"a.b.c"` -- nested field access
/// - `"items[0]"` -- array indexing
/// - `"a.items[2].name"` -- mixed
fn navigate_path_on_value<'a>(start: &DynamicValue<'a>, path: &str) -> DynamicValue<'a> {
    if path.is_empty() {
        return match start {
            DynamicValue::Struct(v) => DynamicValue::Struct(v.clone()),
            _ => DynamicValue::Void,
        };
    }

    // We need to own the intermediate values as we traverse. Clone the start
    // into a local and walk through segments.
    let segments = parse_path(path);
    let mut current = clone_value(start);

    for seg in &segments {
        current = match seg {
            PathSegment::Field(name) => current.field(name),
            PathSegment::Index(idx) => current.get(*idx),
        };
        if !current.exists() {
            return DynamicValue::Void;
        }
    }
    current
}

#[derive(Debug)]
enum PathSegment<'a> {
    Field(&'a str),
    Index(usize),
}

/// Parse a path string into segments.
///
/// `"a.b[0].c"` => [Field("a"), Field("b"), Index(0), Field("c")]
fn parse_path(path: &str) -> Vec<PathSegment<'_>> {
    let mut segments = Vec::new();
    let mut remaining = path;

    while !remaining.is_empty() {
        // Skip leading dots
        if remaining.starts_with('.') {
            remaining = &remaining[1..];
            continue;
        }

        // Check for [index]
        if remaining.starts_with('[') {
            if let Some(end) = remaining.find(']') {
                if let Ok(idx) = remaining[1..end].parse::<usize>() {
                    segments.push(PathSegment::Index(idx));
                }
                remaining = &remaining[end + 1..];
                continue;
            }
        }

        // Find the end of this field name (next dot or bracket)
        let end = remaining
            .find(|c: char| c == '.' || c == '[')
            .unwrap_or(remaining.len());
        if end > 0 {
            segments.push(PathSegment::Field(&remaining[..end]));
        }
        remaining = &remaining[end..];
    }

    segments
}

/// Clone a DynamicValue for path navigation (we need to own intermediates).
fn clone_value<'a>(val: &DynamicValue<'a>) -> DynamicValue<'a> {
    match val {
        DynamicValue::Void => DynamicValue::Void,
        DynamicValue::Bool(v) => DynamicValue::Bool(*v),
        DynamicValue::I8(v) => DynamicValue::I8(*v),
        DynamicValue::I16(v) => DynamicValue::I16(*v),
        DynamicValue::I32(v) => DynamicValue::I32(*v),
        DynamicValue::I64(v) => DynamicValue::I64(*v),
        DynamicValue::U8(v) => DynamicValue::U8(*v),
        DynamicValue::U16(v) => DynamicValue::U16(*v),
        DynamicValue::U32(v) => DynamicValue::U32(*v),
        DynamicValue::U64(v) => DynamicValue::U64(*v),
        DynamicValue::F32(v) => DynamicValue::F32(*v),
        DynamicValue::F64(v) => DynamicValue::F64(*v),
        DynamicValue::Text(v) => DynamicValue::Text(v),
        DynamicValue::Data(v) => DynamicValue::Data(v),
        DynamicValue::Struct(v) => DynamicValue::Struct(v.clone()),
        DynamicValue::List(v) => DynamicValue::List(v.clone()),
    }
}

// ---------------------------------------------------------------------------
// HashedPath — pre-parsed, pre-hashed path for fast repeated queries
// ---------------------------------------------------------------------------

/// A pre-parsed segment in a hashed path.
#[derive(Debug, Clone)]
enum HashedSegment {
    /// A field name with pre-computed xxh64 hash.
    Field(FieldName),
    /// An array index.
    Index(usize),
}

/// A pre-parsed, pre-hashed path for fast repeated lookups.
///
/// Parse and hash once, evaluate many times. Significantly faster than
/// `DynamicView::path()` for repeated queries against different views
/// with the same schema.
///
/// # Example
/// ```
/// use psio::dynamic_view::HashedPath;
///
/// let path = HashedPath::new("a.b[0].c");
/// // path can now be used with view.eval(&path) without re-parsing or re-hashing
/// ```
#[derive(Debug, Clone)]
pub struct HashedPath {
    segments: Vec<HashedSegment>,
}

impl HashedPath {
    /// Parse a dotted path string like `"a.b[0].c"` into pre-hashed segments.
    pub fn new(path: &str) -> Self {
        let raw_segments = parse_path(path);
        let segments = raw_segments
            .into_iter()
            .map(|seg| match seg {
                PathSegment::Field(name) => HashedSegment::Field(FieldName::new(name)),
                PathSegment::Index(idx) => HashedSegment::Index(idx),
            })
            .collect();
        HashedPath { segments }
    }

    /// Number of segments in the path.
    pub fn len(&self) -> usize {
        self.segments.len()
    }

    /// Whether the path is empty.
    pub fn is_empty(&self) -> bool {
        self.segments.is_empty()
    }
}

impl<'a> DynamicView<'a> {
    /// Evaluate a pre-parsed, pre-hashed path without re-parsing or re-hashing.
    ///
    /// This is significantly faster than `path()` for repeated queries because
    /// it avoids string parsing and xxh64 computation on each call.
    pub fn eval(&self, hashed_path: &HashedPath) -> DynamicValue<'a> {
        let mut current = DynamicValue::Struct(self.clone());
        for seg in &hashed_path.segments {
            current = match seg {
                HashedSegment::Field(field_name) => match &current {
                    DynamicValue::Struct(view) => view.field_by_field_name(field_name),
                    _ => DynamicValue::Void,
                },
                HashedSegment::Index(idx) => current.get(*idx),
            };
            if !current.exists() {
                return DynamicValue::Void;
            }
        }
        current
    }
}

impl<'a> DynamicValue<'a> {
    /// Evaluate a pre-parsed, pre-hashed path from this value.
    pub fn eval(&self, hashed_path: &HashedPath) -> DynamicValue<'a> {
        let mut current = clone_value(self);
        for seg in &hashed_path.segments {
            current = match seg {
                HashedSegment::Field(field_name) => match &current {
                    DynamicValue::Struct(view) => view.field_by_field_name(field_name),
                    _ => DynamicValue::Void,
                },
                HashedSegment::Index(idx) => current.get(*idx),
            };
            if !current.exists() {
                return DynamicValue::Void;
            }
        }
        current
    }
}

// ---------------------------------------------------------------------------
// CompiledPath — pre-resolved path against a specific schema
// ---------------------------------------------------------------------------

/// A step in a compiled path, pre-resolved to an index.
#[derive(Debug, Clone)]
enum CompiledStep {
    /// Index into the schema's `sorted_fields` array.
    Field(usize),
    /// Array/list index.
    Index(usize),
}

/// A path pre-resolved against a specific `DynamicSchema`.
///
/// Unlike `HashedPath` which still needs to search the schema's tag array at
/// eval time, `CompiledPath` resolves field names to sorted_fields indices at
/// compile time. Evaluation then uses direct indexing with no hashing or scanning.
///
/// # Example
/// ```
/// use psio::dynamic_schema::{SchemaBuilder, DynamicType};
/// use psio::dynamic_view::{DynamicView, WireFormat, CompiledPath};
/// use psio::wit::pack::WitPack;
/// use psio::wit::layout::compute_struct_layout;
///
/// let (locs, _total, _align) = compute_struct_layout(&[(4, 4), (4, 4)]);
/// let schema = SchemaBuilder::new()
///     .field_scalar("x", DynamicType::U32, locs[0].offset)
///     .field_scalar("y", DynamicType::U32, locs[1].offset)
///     .data_words(1)
///     .build();
///
/// let cp = CompiledPath::compile("x", &schema).unwrap();
/// let buf = (42u32, 99u32).wit_pack();
/// let view = DynamicView::new(&buf, &schema, WireFormat::Wit);
/// assert_eq!(view.eval_compiled(&cp).as_u32(), Some(42));
/// ```
#[derive(Debug, Clone)]
pub struct CompiledPath {
    /// Pre-resolved steps for each path segment.
    steps: Vec<CompiledStep>,
}

impl CompiledPath {
    /// Compile a dotted path string against a specific schema.
    ///
    /// Returns `None` if any field name in the path cannot be resolved
    /// in the corresponding schema.
    pub fn compile(path: &str, schema: &DynamicSchema) -> Option<Self> {
        let raw_segments = parse_path(path);
        let mut steps = Vec::with_capacity(raw_segments.len());
        let mut current_schema = schema;

        for seg in &raw_segments {
            match seg {
                PathSegment::Field(name) => {
                    let hash = xxh64::hash_str(name);
                    // Find the index in sorted_fields
                    let idx = current_schema
                        .sorted_fields
                        .iter()
                        .position(|f| f.name_hash == hash && f.name == *name)?;
                    let field = &current_schema.sorted_fields[idx];
                    steps.push(CompiledStep::Field(idx));

                    // If this field has a nested schema, update current_schema
                    // for subsequent field lookups
                    if let Some(ref nested) = field.nested {
                        current_schema = nested;
                    }
                }
                PathSegment::Index(idx) => {
                    steps.push(CompiledStep::Index(*idx));
                }
            }
        }

        Some(CompiledPath { steps })
    }

    /// Number of steps in the compiled path.
    pub fn len(&self) -> usize {
        self.steps.len()
    }

    /// Whether the compiled path is empty.
    pub fn is_empty(&self) -> bool {
        self.steps.is_empty()
    }
}

impl<'a> DynamicView<'a> {
    /// Evaluate a pre-compiled path without any hashing or searching.
    ///
    /// This is the fastest path evaluation method: all field names have
    /// been resolved to direct indices at compile time.
    pub fn eval_compiled(&self, compiled: &CompiledPath) -> DynamicValue<'a> {
        let mut current = DynamicValue::Struct(self.clone());
        for step in &compiled.steps {
            current = match step {
                CompiledStep::Field(idx) => match &current {
                    DynamicValue::Struct(view) => {
                        let desc = &view.schema.sorted_fields[*idx];
                        view.read_field(desc)
                    }
                    _ => DynamicValue::Void,
                },
                CompiledStep::Index(idx) => current.get(*idx),
            };
            if !current.exists() {
                return DynamicValue::Void;
            }
        }
        current
    }
}

impl<'a> DynamicValue<'a> {
    /// Evaluate a pre-compiled path from this value.
    pub fn eval_compiled(&self, compiled: &CompiledPath) -> DynamicValue<'a> {
        let mut current = clone_value(self);
        for step in &compiled.steps {
            current = match step {
                CompiledStep::Field(idx) => match &current {
                    DynamicValue::Struct(view) => {
                        let desc = &view.schema.sorted_fields[*idx];
                        view.read_field(desc)
                    }
                    _ => DynamicValue::Void,
                },
                CompiledStep::Index(idx) => current.get(*idx),
            };
            if !current.exists() {
                return DynamicValue::Void;
            }
        }
        current
    }
}

// ---------------------------------------------------------------------------
// FromDynamicView — duck-typed extraction
// ---------------------------------------------------------------------------

/// Trait for extracting a typed Rust struct from a `DynamicView` by matching
/// field names.
///
/// Maps DynamicView fields by name onto the implementing type's fields.
/// Returns `None` if any required field is missing or has an incompatible type.
///
/// # Example
/// ```
/// use psio::dynamic_view::{DynamicView, DynamicValue, FromDynamicView};
/// use psio::dynamic_schema::{DynamicSchema, DynamicType, SchemaBuilder};
///
/// struct Point { x: i32, y: i32 }
///
/// impl FromDynamicView for Point {
///     fn from_dynamic_view(view: &DynamicView) -> Option<Self> {
///         Some(Point {
///             x: view.field("x").as_i32()?,
///             y: view.field("y").as_i32()?,
///         })
///     }
/// }
/// ```
pub trait FromDynamicView: Sized {
    fn from_dynamic_view(view: &DynamicView) -> Option<Self>;
}

impl<'a> DynamicView<'a> {
    /// Extract a typed value from this view using duck-typed field matching.
    pub fn as_type<T: FromDynamicView>(&self) -> Option<T> {
        T::from_dynamic_view(self)
    }
}

// ---------------------------------------------------------------------------
// ToDynamicSchema — trait for types that can generate their own schema
// ---------------------------------------------------------------------------

/// Trait for types that can produce a `DynamicSchema` describing their fields.
///
/// This is typically implemented via the `#[derive(ToDynamicSchema)]` macro.
pub trait ToDynamicSchema {
    fn dynamic_schema() -> DynamicSchema;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dynamic_schema::{DynamicType, FieldDesc, SchemaBuilder};
    use crate::wit::layout::compute_struct_layout;
    use crate::wit::pack::WitPack;

    // ── Helper: build a WIT schema for a struct given field types ───────

    /// Build a DynamicSchema for WIT format from a list of (name, type) pairs.
    /// Returns the schema. Offset computation uses WIT natural alignment rules.
    fn wit_schema(fields: &[(&str, DynamicType)]) -> DynamicSchema {
        // Compute layout
        let layout_fields: Vec<(u32, u32)> = fields
            .iter()
            .map(|(_, ty)| {
                let (align, size) = wit_type_layout(*ty);
                (align, size)
            })
            .collect();
        let (locs, _total, _align) = compute_struct_layout(&layout_fields);

        let mut builder = SchemaBuilder::new();
        for (i, (name, ty)) in fields.iter().enumerate() {
            match ty {
                DynamicType::Text | DynamicType::Data => {
                    // Text/Data are pointer-like in most formats, but in WIT
                    // they are inline (ptr, len) at the field offset.
                    // For WIT dynamic view, we store them as non-pointer with
                    // the offset being the location of the (ptr, len) pair.
                    let mut fd = FieldDesc::scalar(name, *ty, locs[i].offset);
                    fd.is_ptr = false; // WIT stores strings inline
                    fd.byte_size = 8; // ptr(4) + len(4)
                    builder = builder.field(fd);
                }
                _ => {
                    builder = builder.field_scalar(name, *ty, locs[i].offset);
                }
            }
        }
        builder.data_words((_total / 8 + 1) as u16).build()
    }

    /// Return (alignment, size) for a WIT DynamicType.
    fn wit_type_layout(ty: DynamicType) -> (u32, u32) {
        match ty {
            DynamicType::Bool | DynamicType::U8 | DynamicType::I8 => (1, 1),
            DynamicType::U16 | DynamicType::I16 => (2, 2),
            DynamicType::U32 | DynamicType::I32 | DynamicType::F32 => (4, 4),
            DynamicType::U64 | DynamicType::I64 | DynamicType::F64 => (8, 8),
            DynamicType::Text | DynamicType::Data => (4, 8), // (ptr, len)
            _ => (1, 0),
        }
    }

    // ── Test 1: basic scalar field access ──────────────────────────────

    #[test]
    fn dynamic_view_wit_scalars() {
        let schema = wit_schema(&[
            ("a", DynamicType::U32),
            ("b", DynamicType::U64),
            ("c", DynamicType::I32),
        ]);

        // Pack: struct { a: u32, b: u64, c: i32 }
        // WIT layout: a at 0 (4 bytes), pad 4, b at 8 (8 bytes), c at 16 (4 bytes)
        // Total: 20, padded to 24 (alignment 8)
        let buf = (42u32, 0x1234_5678_9ABCu64, -7i32).wit_pack();

        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);

        assert_eq!(view.field("a").as_u32(), Some(42));
        assert_eq!(view.field("b").as_u64(), Some(0x1234_5678_9ABC));
        assert_eq!(view.field("c").as_i32(), Some(-7));
        assert_eq!(view.field("nonexistent").exists(), false);
    }

    // ── Test 2: DynamicValue conversions ───────────────────────────────

    #[test]
    fn dynamic_value_conversions() {
        // to_u64 widening
        assert_eq!(DynamicValue::U8(255).to_u64(), Some(255));
        assert_eq!(DynamicValue::U16(1000).to_u64(), Some(1000));
        assert_eq!(DynamicValue::U32(42).to_u64(), Some(42));
        assert_eq!(DynamicValue::I32(-1).to_u64(), Some(-1i32 as u64));
        assert_eq!(DynamicValue::Bool(true).to_u64(), Some(1));

        // to_f64 widening
        assert_eq!(DynamicValue::F32(3.14f32).to_f64(), Some(3.14f32 as f64));
        assert_eq!(DynamicValue::U32(100).to_f64(), Some(100.0));
        assert_eq!(DynamicValue::I64(-42).to_f64(), Some(-42.0));

        // as_* exact match
        assert_eq!(DynamicValue::Bool(true).as_bool(), Some(true));
        assert_eq!(DynamicValue::I64(99).as_i64(), Some(99));
        assert_eq!(DynamicValue::U32(7).as_i32(), None); // wrong type

        // exists
        assert!(DynamicValue::U32(0).exists());
        assert!(!DynamicValue::Void.exists());

        // kind
        assert_eq!(DynamicValue::U32(0).kind(), DynamicType::U32);
        assert_eq!(DynamicValue::Text("hi").kind(), DynamicType::Text);
        assert_eq!(DynamicValue::Void.kind(), DynamicType::Void);
    }

    // ── Test 3: string field access ────────────────────────────────────

    #[test]
    fn dynamic_view_wit_string() {
        // struct { id: u32, name: String }
        let schema = wit_schema(&[
            ("id", DynamicType::U32),
            ("name", DynamicType::Text),
        ]);

        let buf = (42u32, String::from("hello")).wit_pack();

        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);
        assert_eq!(view.field("id").as_u32(), Some(42));
        assert_eq!(view.field("name").as_str(), Some("hello"));
    }

    // ── Test 4: bool and small types ───────────────────────────────────

    #[test]
    fn dynamic_view_wit_bool_and_u8() {
        // struct { flag: bool, value: u8 }
        let schema = wit_schema(&[
            ("flag", DynamicType::Bool),
            ("value", DynamicType::U8),
        ]);

        let buf = (true, 0xFFu8).wit_pack();

        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);
        assert_eq!(view.field("flag").as_bool(), Some(true));
        assert_eq!(view.field("value").as_u8(), Some(0xFF));
    }

    // ── Test 5: nested struct (path navigation) ────────────────────────

    #[test]
    fn dynamic_view_wit_nested_struct() {
        // Inner: struct { x: i32, y: i32 }
        let inner_fields: Vec<(u32, u32)> = vec![(4, 4), (4, 4)];
        let (inner_locs, inner_total, _) = compute_struct_layout(&inner_fields);

        let inner_schema = SchemaBuilder::new()
            .field_scalar("x", DynamicType::I32, inner_locs[0].offset)
            .field_scalar("y", DynamicType::I32, inner_locs[1].offset)
            .data_words(1)
            .build();

        // Outer: struct { id: u32, pos: Inner }
        // WIT layout for outer: u32(4) at 0, Inner(8) at 4 (alignment 4)
        let outer_fields: Vec<(u32, u32)> = vec![(4, 4), (4, inner_total)];
        let (outer_locs, _outer_total, _) = compute_struct_layout(&outer_fields);

        // For WIT, nested structs are inline, so is_ptr is false and offset
        // is the inline byte offset.
        let mut pos_fd = FieldDesc::scalar("pos", DynamicType::Struct, outer_locs[1].offset);
        pos_fd.nested = Some(Box::new(inner_schema));
        pos_fd.byte_size = inner_total as u8;

        let outer_schema = SchemaBuilder::new()
            .field_scalar("id", DynamicType::U32, outer_locs[0].offset)
            .field(pos_fd)
            .data_words(2)
            .build();

        // Pack: (id=1, (x=10, y=20))
        let buf = (1u32, (10i32, 20i32)).wit_pack();

        let view = DynamicView::new(&buf, &outer_schema, WireFormat::Wit);

        // Direct field access
        assert_eq!(view.field("id").as_u32(), Some(1));

        // Nested field access via chaining
        assert_eq!(view.field("pos").field("x").as_i32(), Some(10));
        assert_eq!(view.field("pos").field("y").as_i32(), Some(20));

        // Path navigation
        assert_eq!(view.path("pos.x").as_i32(), Some(10));
        assert_eq!(view.path("pos.y").as_i32(), Some(20));
    }

    // ── Test 6: list access via vector field ───────────────────────────

    #[test]
    fn dynamic_view_wit_list() {
        // struct { items: Vec<u32> }
        // WIT Vec is (ptr, len) = 8 bytes at alignment 4.
        let mut fd = FieldDesc::scalar("items", DynamicType::Vector, 0);
        fd.is_ptr = false;
        fd.byte_size = 4; // element size for u32
        fd.nested = None;

        let schema = SchemaBuilder::new()
            .field(fd)
            .data_words(1)
            .build();

        let items: Vec<u32> = vec![100, 200, 300];
        let buf = items.wit_pack();

        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);
        let list_val = view.field("items");

        match &list_val {
            DynamicValue::List(lv) => {
                assert_eq!(lv.len(), 3);
                assert_eq!(lv.get(0).as_u32(), Some(100));
                assert_eq!(lv.get(1).as_u32(), Some(200));
                assert_eq!(lv.get(2).as_u32(), Some(300));
                assert_eq!(lv.get(3).exists(), false); // out of bounds
            }
            other => panic!("expected List, got {:?}", other),
        }
    }

    // ── Test 7: list iteration ─────────────────────────────────────────

    #[test]
    fn dynamic_view_wit_list_iter() {
        let mut fd = FieldDesc::scalar("vals", DynamicType::Vector, 0);
        fd.is_ptr = false;
        fd.byte_size = 4; // u32 elements
        fd.nested = None;

        let schema = SchemaBuilder::new().field(fd).data_words(1).build();

        let items: Vec<u32> = vec![10, 20, 30];
        let buf = items.wit_pack();

        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);
        if let DynamicValue::List(lv) = view.field("vals") {
            let collected: Vec<u32> = lv.iter().filter_map(|v| v.as_u32()).collect();
            assert_eq!(collected, vec![10, 20, 30]);
        } else {
            panic!("expected List");
        }
    }

    // ── Test 8: path with array index ──────────────────────────────────

    #[test]
    fn dynamic_view_wit_path_with_index() {
        // struct { items: Vec<u32> }
        let mut fd = FieldDesc::scalar("items", DynamicType::Vector, 0);
        fd.is_ptr = false;
        fd.byte_size = 4;
        fd.nested = None;

        let schema = SchemaBuilder::new().field(fd).data_words(1).build();

        let items: Vec<u32> = vec![111, 222, 333];
        let buf = items.wit_pack();

        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);

        assert_eq!(view.path("items[0]").as_u32(), Some(111));
        assert_eq!(view.path("items[1]").as_u32(), Some(222));
        assert_eq!(view.path("items[2]").as_u32(), Some(333));
        assert_eq!(view.path("items[3]").exists(), false);
    }

    // ── Test 9: field_names and field_count ────────────────────────────

    #[test]
    fn dynamic_view_field_names() {
        let schema = wit_schema(&[
            ("alpha", DynamicType::U32),
            ("beta", DynamicType::I64),
            ("gamma", DynamicType::F64),
        ]);

        let buf = (1u32, 2i64, 3.0f64).wit_pack();
        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);

        assert_eq!(view.field_count(), 3);
        let names: Vec<&str> = view.field_names().collect();
        assert_eq!(names, vec!["alpha", "beta", "gamma"]);
    }

    // ── Test 10: FieldName pre-computed hash lookup ────────────────────

    #[test]
    fn dynamic_view_field_by_field_name() {
        let schema = wit_schema(&[
            ("score", DynamicType::U32),
        ]);

        let buf = (42u32,).wit_pack();
        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);

        let fn_ = FieldName::new("score");
        assert_eq!(view.field_by_field_name(&fn_).as_u32(), Some(42));
    }

    // ── Test 11: float fields ──────────────────────────────────────────

    #[test]
    fn dynamic_view_wit_floats() {
        let schema = wit_schema(&[
            ("f", DynamicType::F32),
            ("d", DynamicType::F64),
        ]);

        let buf = (3.14f32, 2.718281828f64).wit_pack();
        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);

        let f = view.field("f").as_f32().unwrap();
        assert!((f - 3.14).abs() < 1e-6);

        let d = view.field("d").as_f64().unwrap();
        assert!((d - 2.718281828).abs() < 1e-9);
    }

    // ── Test 12: parse_path unit test ──────────────────────────────────

    #[test]
    fn test_parse_path() {
        let segs = parse_path("a.b.c");
        assert_eq!(segs.len(), 3);
        assert!(matches!(segs[0], PathSegment::Field("a")));
        assert!(matches!(segs[1], PathSegment::Field("b")));
        assert!(matches!(segs[2], PathSegment::Field("c")));

        let segs = parse_path("items[0]");
        assert_eq!(segs.len(), 2);
        assert!(matches!(segs[0], PathSegment::Field("items")));
        assert!(matches!(segs[1], PathSegment::Index(0)));

        let segs = parse_path("a.items[2].name");
        assert_eq!(segs.len(), 4);
        assert!(matches!(segs[0], PathSegment::Field("a")));
        assert!(matches!(segs[1], PathSegment::Field("items")));
        assert!(matches!(segs[2], PathSegment::Index(2)));
        assert!(matches!(segs[3], PathSegment::Field("name")));

        let segs = parse_path("");
        assert_eq!(segs.len(), 0);
    }

    // ── Test 13: complex nested path ───────────────────────────────────

    #[test]
    fn dynamic_view_wit_deep_path() {
        // struct Inner { val: u32 }
        let inner_locs_data: Vec<(u32, u32)> = vec![(4, 4)];
        let (inner_locs, inner_total, _) = compute_struct_layout(&inner_locs_data);

        let inner_schema = SchemaBuilder::new()
            .field_scalar("val", DynamicType::U32, inner_locs[0].offset)
            .data_words(1)
            .build();

        // struct Mid { inner: Inner }
        let mid_locs_data: Vec<(u32, u32)> = vec![(4, inner_total)];
        let (mid_locs, mid_total, _) = compute_struct_layout(&mid_locs_data);

        let mut mid_inner_fd =
            FieldDesc::scalar("inner", DynamicType::Struct, mid_locs[0].offset);
        mid_inner_fd.nested = Some(Box::new(inner_schema));
        mid_inner_fd.byte_size = inner_total as u8;

        let mid_schema = SchemaBuilder::new()
            .field(mid_inner_fd)
            .data_words(1)
            .build();

        // struct Outer { mid: Mid }
        let outer_locs_data: Vec<(u32, u32)> = vec![(4, mid_total)];
        let (outer_locs, _outer_total, _) = compute_struct_layout(&outer_locs_data);

        let mut outer_mid_fd =
            FieldDesc::scalar("mid", DynamicType::Struct, outer_locs[0].offset);
        outer_mid_fd.nested = Some(Box::new(mid_schema));
        outer_mid_fd.byte_size = mid_total as u8;

        let outer_schema = SchemaBuilder::new()
            .field(outer_mid_fd)
            .data_words(1)
            .build();

        // Pack a nested tuple that matches: Outer { mid: Mid { inner: Inner { val: 42 } } }
        // Since all inner structs are just u32, the data is just u32.
        let buf = (42u32,).wit_pack();

        let view = DynamicView::new(&buf, &outer_schema, WireFormat::Wit);

        // Navigate deeply
        assert_eq!(view.path("mid.inner.val").as_u32(), Some(42));
        assert_eq!(view.field("mid").field("inner").field("val").as_u32(), Some(42));
    }

    // ── Test 14: DynamicValue::path on a Struct value ──────────────────

    #[test]
    fn dynamic_value_path_on_struct() {
        let schema = wit_schema(&[
            ("x", DynamicType::U32),
            ("y", DynamicType::U32),
        ]);

        let buf = (10u32, 20u32).wit_pack();
        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);

        let val = DynamicValue::Struct(view);
        assert_eq!(val.path("x").as_u32(), Some(10));
        assert_eq!(val.path("y").as_u32(), Some(20));
        assert_eq!(val.path("z").exists(), false);
    }

    // ── Test 15: view kind ─────────────────────────────────────────────

    #[test]
    fn dynamic_view_kind() {
        let schema = wit_schema(&[("a", DynamicType::U32)]);
        let buf = (1u32,).wit_pack();
        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);
        assert_eq!(view.kind(), DynamicType::Struct);
    }

    // ══════════════════════════════════════════════════════════════════════
    // Cap'n Proto dynamic view tests
    // ══════════════════════════════════════════════════════════════════════

    #[test]
    fn dynamic_view_capnp_scalars() {
        use crate::capnp::layout::{FieldKind, MemberDesc};
        use crate::capnp::pack::{pack_data_field, WordBuf, CapnpPack};

        // Build a capnp message: struct { a: u32, b: u64, c: bool }
        struct TestStruct;
        impl CapnpPack for TestStruct {
            fn member_descs() -> Vec<MemberDesc> {
                vec![
                    MemberDesc::Simple(FieldKind::Scalar(4)),  // u32
                    MemberDesc::Simple(FieldKind::Scalar(8)),  // u64
                    MemberDesc::Simple(FieldKind::Bool),       // bool
                ]
            }
            fn pack_into(&self, _buf: &mut WordBuf, _ds: u32, _ps: u32) {}
        }

        let layout = TestStruct::capnp_layout();

        // Build message manually
        let mut buf = WordBuf::new();
        let root_ptr = buf.alloc(1);
        let data_start = buf.alloc(layout.data_words as u32);
        let _ptrs_start = buf.alloc(layout.ptr_count as u32);
        buf.write_struct_ptr(root_ptr, data_start, layout.data_words, layout.ptr_count);
        pack_data_field(&mut buf, data_start, &layout.fields[0], 42u32);
        pack_data_field(&mut buf, data_start, &layout.fields[1], 0xDEAD_BEEF_u64);
        crate::capnp::pack::pack_bool_field(&mut buf, data_start, &layout.fields[2], true);
        let msg = buf.finish();

        // Build dynamic schema matching the capnp layout
        let schema = SchemaBuilder::new()
            .field({
                let mut fd = FieldDesc::scalar("a", DynamicType::U32, layout.fields[0].offset);
                fd.is_ptr = layout.fields[0].is_ptr;
                fd
            })
            .field({
                let mut fd = FieldDesc::scalar("b", DynamicType::U64, layout.fields[1].offset);
                fd.is_ptr = layout.fields[1].is_ptr;
                fd
            })
            .field({
                let mut fd = FieldDesc::bool_field("c", layout.fields[2].offset, layout.fields[2].bit_index);
                fd.is_ptr = layout.fields[2].is_ptr;
                fd
            })
            .data_words(layout.data_words)
            .ptr_count(layout.ptr_count)
            .build();

        let view = DynamicView::new(&msg, &schema, WireFormat::Capnp);
        assert_eq!(view.field("a").as_u32(), Some(42));
        assert_eq!(view.field("b").as_u64(), Some(0xDEAD_BEEF));
        assert_eq!(view.field("c").as_bool(), Some(true));
        assert!(!view.field("nonexistent").exists());
    }

    #[test]
    fn dynamic_view_capnp_text() {
        use crate::capnp::layout::{FieldKind, MemberDesc};
        use crate::capnp::pack::{pack_text_field, WordBuf, CapnpPack, pack_data_field};

        struct TestStruct;
        impl CapnpPack for TestStruct {
            fn member_descs() -> Vec<MemberDesc> {
                vec![
                    MemberDesc::Simple(FieldKind::Scalar(4)),  // u32
                    MemberDesc::Simple(FieldKind::Pointer),    // text
                ]
            }
            fn pack_into(&self, _buf: &mut WordBuf, _ds: u32, _ps: u32) {}
        }

        let layout = TestStruct::capnp_layout();

        let mut buf = WordBuf::new();
        let root_ptr = buf.alloc(1);
        let data_start = buf.alloc(layout.data_words as u32);
        let ptrs_start = buf.alloc(layout.ptr_count as u32);
        buf.write_struct_ptr(root_ptr, data_start, layout.data_words, layout.ptr_count);
        pack_data_field(&mut buf, data_start, &layout.fields[0], 99u32);
        pack_text_field(&mut buf, ptrs_start, &layout.fields[1], "hello capnp");
        let msg = buf.finish();

        let schema = SchemaBuilder::new()
            .field({
                let mut fd = FieldDesc::scalar("id", DynamicType::U32, layout.fields[0].offset);
                fd.is_ptr = layout.fields[0].is_ptr;
                fd
            })
            .field({
                let mut fd = FieldDesc::pointer("name", DynamicType::Text, layout.fields[1].offset);
                fd.is_ptr = true;
                fd
            })
            .data_words(layout.data_words)
            .ptr_count(layout.ptr_count)
            .build();

        let view = DynamicView::new(&msg, &schema, WireFormat::Capnp);
        assert_eq!(view.field("id").as_u32(), Some(99));
        assert_eq!(view.field("name").as_str(), Some("hello capnp"));
    }

    #[test]
    fn dynamic_view_capnp_nested_struct() {
        use crate::capnp::layout::{FieldKind, MemberDesc};
        use crate::capnp::pack::{pack_data_field, WordBuf, CapnpPack};

        // Inner: { x: i32, y: i32 }
        struct InnerLayout;
        impl CapnpPack for InnerLayout {
            fn member_descs() -> Vec<MemberDesc> {
                vec![
                    MemberDesc::Simple(FieldKind::Scalar(4)),
                    MemberDesc::Simple(FieldKind::Scalar(4)),
                ]
            }
            fn pack_into(&self, _: &mut WordBuf, _: u32, _: u32) {}
        }
        let inner_layout = InnerLayout::capnp_layout();

        // Outer: { id: u32, pos: Inner }
        struct OuterLayout;
        impl CapnpPack for OuterLayout {
            fn member_descs() -> Vec<MemberDesc> {
                vec![
                    MemberDesc::Simple(FieldKind::Scalar(4)),
                    MemberDesc::Simple(FieldKind::Pointer),
                ]
            }
            fn pack_into(&self, _: &mut WordBuf, _: u32, _: u32) {}
        }
        let outer_layout = OuterLayout::capnp_layout();

        // Build message
        let mut buf = WordBuf::new();
        let root_ptr = buf.alloc(1);
        let data_start = buf.alloc(outer_layout.data_words as u32);
        let ptrs_start = buf.alloc(outer_layout.ptr_count as u32);
        buf.write_struct_ptr(root_ptr, data_start, outer_layout.data_words, outer_layout.ptr_count);
        pack_data_field(&mut buf, data_start, &outer_layout.fields[0], 7u32);

        // Write inner struct
        let inner_data = buf.alloc(inner_layout.data_words as u32);
        let inner_ptrs = buf.alloc(inner_layout.ptr_count as u32);
        let _ = inner_ptrs; // no pointer fields
        buf.write_struct_ptr(
            ptrs_start + outer_layout.fields[1].offset,
            inner_data,
            inner_layout.data_words,
            inner_layout.ptr_count,
        );
        pack_data_field(&mut buf, inner_data, &inner_layout.fields[0], 10i32);
        pack_data_field(&mut buf, inner_data, &inner_layout.fields[1], 20i32);

        let msg = buf.finish();

        // Build nested schemas
        let inner_schema = SchemaBuilder::new()
            .field({
                let mut fd = FieldDesc::scalar("x", DynamicType::I32, inner_layout.fields[0].offset);
                fd.is_ptr = false;
                fd
            })
            .field({
                let mut fd = FieldDesc::scalar("y", DynamicType::I32, inner_layout.fields[1].offset);
                fd.is_ptr = false;
                fd
            })
            .data_words(inner_layout.data_words)
            .ptr_count(inner_layout.ptr_count)
            .build();

        let outer_schema = SchemaBuilder::new()
            .field({
                let mut fd = FieldDesc::scalar("id", DynamicType::U32, outer_layout.fields[0].offset);
                fd.is_ptr = false;
                fd
            })
            .field({
                let mut fd = FieldDesc::struct_field("pos", outer_layout.fields[1].offset, inner_schema);
                fd.is_ptr = true;
                fd
            })
            .data_words(outer_layout.data_words)
            .ptr_count(outer_layout.ptr_count)
            .build();

        let view = DynamicView::new(&msg, &outer_schema, WireFormat::Capnp);
        assert_eq!(view.field("id").as_u32(), Some(7));
        assert_eq!(view.path("pos.x").as_i32(), Some(10));
        assert_eq!(view.path("pos.y").as_i32(), Some(20));
    }

    // ══════════════════════════════════════════════════════════════════════
    // FlatBuffers dynamic view tests
    // ══════════════════════════════════════════════════════════════════════

    #[test]
    fn dynamic_view_flatbuf_scalars() {
        use crate::flatbuf::builder::FbBuilder;

        let mut b = FbBuilder::default();
        b.start_table();
        b.add_scalar_u32(4, 42, 0);   // slot 0
        b.add_scalar_u64(6, 0xCAFE, 0); // slot 1
        b.add_scalar_u8_force(8, 1);  // slot 2 (bool as u8)
        let root = b.end_table();
        b.finish(root);
        let data = b.data().to_vec();

        // slot index = (vt_byte - 4) / 2
        // vt_byte 4 -> slot 0, vt_byte 6 -> slot 1, vt_byte 8 -> slot 2
        let schema = SchemaBuilder::new()
            .field({
                let mut fd = FieldDesc::scalar("a", DynamicType::U32, 0); // slot 0
                fd.is_ptr = false;
                fd
            })
            .field({
                let mut fd = FieldDesc::scalar("b", DynamicType::U64, 1); // slot 1
                fd.is_ptr = false;
                fd
            })
            .field({
                let mut fd = FieldDesc::scalar("c", DynamicType::Bool, 2); // slot 2
                fd.is_ptr = false;
                fd
            })
            .build();

        let view = DynamicView::new(&data, &schema, WireFormat::Flatbuf);
        assert_eq!(view.field("a").as_u32(), Some(42));
        assert_eq!(view.field("b").as_u64(), Some(0xCAFE));
        assert_eq!(view.field("c").as_bool(), Some(true));
        assert!(!view.field("nonexistent").exists());
    }

    #[test]
    fn dynamic_view_flatbuf_string() {
        use crate::flatbuf::builder::FbBuilder;

        let mut b = FbBuilder::default();
        let s_off = b.create_string("hello flatbuf");
        b.start_table();
        b.add_scalar_u32(4, 7, 0);    // slot 0: id
        b.add_offset_field(6, s_off);  // slot 1: name
        let root = b.end_table();
        b.finish(root);
        let data = b.data().to_vec();

        let schema = SchemaBuilder::new()
            .field({
                let mut fd = FieldDesc::scalar("id", DynamicType::U32, 0);
                fd.is_ptr = false;
                fd
            })
            .field({
                let mut fd = FieldDesc::pointer("name", DynamicType::Text, 1);
                fd.is_ptr = true;
                fd
            })
            .build();

        let view = DynamicView::new(&data, &schema, WireFormat::Flatbuf);
        assert_eq!(view.field("id").as_u32(), Some(7));
        assert_eq!(view.field("name").as_str(), Some("hello flatbuf"));
    }

    #[test]
    fn dynamic_view_flatbuf_nested_table() {
        use crate::flatbuf::builder::FbBuilder;

        let mut b = FbBuilder::default();

        // Build inner table: { x: i32=10, y: i32=20 }
        b.start_table();
        b.add_scalar_i32(4, 10, 0);  // slot 0: x
        b.add_scalar_i32(6, 20, 0);  // slot 1: y
        let inner = b.end_table();

        // Build outer table: { id: u32=5, pos: inner }
        b.start_table();
        b.add_scalar_u32(4, 5, 0);     // slot 0: id
        b.add_offset_field(6, inner);   // slot 1: pos
        let root = b.end_table();
        b.finish(root);
        let data = b.data().to_vec();

        let inner_schema = SchemaBuilder::new()
            .field({
                let mut fd = FieldDesc::scalar("x", DynamicType::I32, 0);
                fd.is_ptr = false;
                fd
            })
            .field({
                let mut fd = FieldDesc::scalar("y", DynamicType::I32, 1);
                fd.is_ptr = false;
                fd
            })
            .build();

        let schema = SchemaBuilder::new()
            .field({
                let mut fd = FieldDesc::scalar("id", DynamicType::U32, 0);
                fd.is_ptr = false;
                fd
            })
            .field({
                let mut fd = FieldDesc::struct_field("pos", 1, inner_schema);
                fd.is_ptr = true;
                fd
            })
            .build();

        let view = DynamicView::new(&data, &schema, WireFormat::Flatbuf);
        assert_eq!(view.field("id").as_u32(), Some(5));
        assert_eq!(view.path("pos.x").as_i32(), Some(10));
        assert_eq!(view.path("pos.y").as_i32(), Some(20));
    }

    #[test]
    fn dynamic_view_flatbuf_vector() {
        use crate::flatbuf::builder::FbBuilder;

        // Build a vector of u32 values
        let values: Vec<u32> = vec![100, 200, 300];
        let mut b = FbBuilder::default();
        let bytes: Vec<u8> = values.iter().flat_map(|v| v.to_le_bytes()).collect();
        let vec_off = b.create_vec_scalar(&bytes, 4, 3);

        b.start_table();
        b.add_offset_field(4, vec_off);  // slot 0: items
        let root = b.end_table();
        b.finish(root);
        let data = b.data().to_vec();

        let schema = SchemaBuilder::new()
            .field({
                let mut fd = FieldDesc::pointer("items", DynamicType::Vector, 0);
                fd.is_ptr = true;
                fd.byte_size = 4; // element size
                fd
            })
            .build();

        let view = DynamicView::new(&data, &schema, WireFormat::Flatbuf);
        let list_val = view.field("items");
        match &list_val {
            DynamicValue::List(lv) => {
                assert_eq!(lv.len(), 3);
                assert_eq!(lv.get(0).as_u32(), Some(100));
                assert_eq!(lv.get(1).as_u32(), Some(200));
                assert_eq!(lv.get(2).as_u32(), Some(300));
                assert!(!lv.get(3).exists());
            }
            other => panic!("expected List, got {:?}", other),
        }
    }

    #[test]
    fn dynamic_view_flatbuf_absent_defaults() {
        use crate::flatbuf::builder::FbBuilder;

        let mut b = FbBuilder::default();
        b.start_table();
        // Only add slot 1, leave slot 0 absent
        b.add_scalar_u32(6, 99, 0); // slot 1
        let root = b.end_table();
        b.finish(root);
        let data = b.data().to_vec();

        let schema = SchemaBuilder::new()
            .field({
                let mut fd = FieldDesc::scalar("missing", DynamicType::U32, 0);
                fd.is_ptr = false;
                fd
            })
            .field({
                let mut fd = FieldDesc::scalar("present", DynamicType::U32, 1);
                fd.is_ptr = false;
                fd
            })
            .build();

        let view = DynamicView::new(&data, &schema, WireFormat::Flatbuf);
        // Absent field should return 0 (default)
        assert_eq!(view.field("missing").as_u32(), Some(0));
        assert_eq!(view.field("present").as_u32(), Some(99));
    }

    // ══════════════════════════════════════════════════════════════════════
    // HashedPath tests
    // ══════════════════════════════════════════════════════════════════════

    #[test]
    fn dynamic_view_hashed_path_basic() {
        let schema = wit_schema(&[
            ("x", DynamicType::U32),
            ("y", DynamicType::U32),
        ]);
        let buf = (10u32, 20u32).wit_pack();
        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);

        let path_x = HashedPath::new("x");
        let path_y = HashedPath::new("y");
        let path_z = HashedPath::new("z");

        assert_eq!(view.eval(&path_x).as_u32(), Some(10));
        assert_eq!(view.eval(&path_y).as_u32(), Some(20));
        assert!(!view.eval(&path_z).exists());
    }

    #[test]
    fn dynamic_view_hashed_path_nested() {
        // Inner: { val: u32 }
        let inner_fields: Vec<(u32, u32)> = vec![(4, 4)];
        let (inner_locs, inner_total, _) = compute_struct_layout(&inner_fields);

        let inner_schema = SchemaBuilder::new()
            .field_scalar("val", DynamicType::U32, inner_locs[0].offset)
            .data_words(1)
            .build();

        // Outer: { inner: Inner }
        let outer_fields: Vec<(u32, u32)> = vec![(4, inner_total)];
        let (outer_locs, _outer_total, _) = compute_struct_layout(&outer_fields);

        let mut inner_fd = FieldDesc::scalar("inner", DynamicType::Struct, outer_locs[0].offset);
        inner_fd.nested = Some(Box::new(inner_schema));
        inner_fd.byte_size = inner_total as u8;

        let outer_schema = SchemaBuilder::new()
            .field(inner_fd)
            .data_words(1)
            .build();

        let buf = (42u32,).wit_pack();
        let view = DynamicView::new(&buf, &outer_schema, WireFormat::Wit);

        let path = HashedPath::new("inner.val");
        assert_eq!(view.eval(&path).as_u32(), Some(42));
    }

    #[test]
    fn dynamic_view_hashed_path_with_index() {
        let mut fd = FieldDesc::scalar("items", DynamicType::Vector, 0);
        fd.is_ptr = false;
        fd.byte_size = 4;
        fd.nested = None;

        let schema = SchemaBuilder::new().field(fd).data_words(1).build();

        let items: Vec<u32> = vec![111, 222, 333];
        let buf = items.wit_pack();

        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);

        let path = HashedPath::new("items[1]");
        assert_eq!(view.eval(&path).as_u32(), Some(222));

        let path_oob = HashedPath::new("items[10]");
        assert!(!view.eval(&path_oob).exists());
    }

    #[test]
    fn dynamic_view_hashed_path_same_as_string_path() {
        let schema = wit_schema(&[
            ("a", DynamicType::U32),
            ("b", DynamicType::I64),
        ]);
        let buf = (42u32, -7i64).wit_pack();
        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);

        // HashedPath and string path should produce identical results
        let hp = HashedPath::new("a");
        assert_eq!(view.eval(&hp).as_u32(), view.path("a").as_u32());

        let hp = HashedPath::new("b");
        assert_eq!(view.eval(&hp).as_i64(), view.path("b").as_i64());
    }

    #[test]
    fn dynamic_view_hashed_path_value_eval() {
        let schema = wit_schema(&[
            ("x", DynamicType::U32),
        ]);
        let buf = (55u32,).wit_pack();
        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);

        let val = DynamicValue::Struct(view);
        let path = HashedPath::new("x");
        assert_eq!(val.eval(&path).as_u32(), Some(55));
    }

    // ══════════════════════════════════════════════════════════════════════
    // Duck-typed extraction tests
    // ══════════════════════════════════════════════════════════════════════

    #[test]
    fn dynamic_view_from_dynamic_view() {
        #[derive(Debug, PartialEq)]
        struct Point {
            x: i32,
            y: i32,
        }

        impl FromDynamicView for Point {
            fn from_dynamic_view(view: &DynamicView) -> Option<Self> {
                Some(Point {
                    x: view.field("x").as_i32()?,
                    y: view.field("y").as_i32()?,
                })
            }
        }

        let inner_fields: Vec<(u32, u32)> = vec![(4, 4), (4, 4)];
        let (locs, _total, _align) = compute_struct_layout(&inner_fields);

        let schema = SchemaBuilder::new()
            .field_scalar("x", DynamicType::I32, locs[0].offset)
            .field_scalar("y", DynamicType::I32, locs[1].offset)
            .data_words(1)
            .build();

        let buf = (10i32, 20i32).wit_pack();
        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);

        let point: Point = view.as_type().unwrap();
        assert_eq!(point, Point { x: 10, y: 20 });
    }

    #[test]
    fn dynamic_view_from_dynamic_view_missing_field() {
        struct NeedsZ {
            _z: u32,
        }

        impl FromDynamicView for NeedsZ {
            fn from_dynamic_view(view: &DynamicView) -> Option<Self> {
                Some(NeedsZ {
                    _z: view.field("z").as_u32()?,
                })
            }
        }

        let schema = wit_schema(&[("x", DynamicType::U32)]);
        let buf = (42u32,).wit_pack();
        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);

        let result: Option<NeedsZ> = view.as_type();
        assert!(result.is_none());
    }

    #[test]
    fn dynamic_view_from_dynamic_view_with_string() {
        #[derive(Debug, PartialEq)]
        struct Named {
            id: u32,
            name: String,
        }

        impl FromDynamicView for Named {
            fn from_dynamic_view(view: &DynamicView) -> Option<Self> {
                Some(Named {
                    id: view.field("id").as_u32()?,
                    name: view.field("name").as_str()?.to_owned(),
                })
            }
        }

        let schema = wit_schema(&[
            ("id", DynamicType::U32),
            ("name", DynamicType::Text),
        ]);

        let buf = (42u32, String::from("test")).wit_pack();
        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);

        let named: Named = view.as_type().unwrap();
        assert_eq!(named, Named { id: 42, name: "test".into() });
    }

    #[test]
    fn dynamic_view_duck_typed_across_formats() {
        // Demonstrate duck-typed extraction from a flatbuf
        use crate::flatbuf::builder::FbBuilder;

        #[derive(Debug, PartialEq)]
        struct Scored {
            score: u32,
        }

        impl FromDynamicView for Scored {
            fn from_dynamic_view(view: &DynamicView) -> Option<Self> {
                Some(Scored {
                    score: view.field("score").as_u32()?,
                })
            }
        }

        let mut b = FbBuilder::default();
        b.start_table();
        b.add_scalar_u32(4, 999, 0);  // slot 0: score
        let root = b.end_table();
        b.finish(root);
        let data = b.data().to_vec();

        let schema = SchemaBuilder::new()
            .field({
                let mut fd = FieldDesc::scalar("score", DynamicType::U32, 0);
                fd.is_ptr = false;
                fd
            })
            .build();

        let view = DynamicView::new(&data, &schema, WireFormat::Flatbuf);
        let scored: Scored = view.as_type().unwrap();
        assert_eq!(scored, Scored { score: 999 });
    }

    // ══════════════════════════════════════════════════════════════════════
    // ToDynamicSchema trait test
    // ══════════════════════════════════════════════════════════════════════

    #[test]
    fn dynamic_view_to_dynamic_schema_manual_impl() {
        // Demonstrate manual ToDynamicSchema implementation
        struct MyPoint;

        impl ToDynamicSchema for MyPoint {
            fn dynamic_schema() -> crate::dynamic_schema::DynamicSchema {
                SchemaBuilder::new()
                    .field_scalar("x", DynamicType::I32, 0)
                    .field_scalar("y", DynamicType::I32, 4)
                    .data_words(1)
                    .build()
            }
        }

        let schema = MyPoint::dynamic_schema();
        assert_eq!(schema.field_count(), 2);
        assert!(schema.find_by_name("x").is_some());
        assert!(schema.find_by_name("y").is_some());

        let fx = schema.find_by_name("x").unwrap();
        assert_eq!(fx.ty, DynamicType::I32);
        assert_eq!(fx.offset, 0);

        let fy = schema.find_by_name("y").unwrap();
        assert_eq!(fy.ty, DynamicType::I32);
        assert_eq!(fy.offset, 4);
    }

    // ══════════════════════════════════════════════════════════════════════
    // HashedPath construction and properties
    // ══════════════════════════════════════════════════════════════════════

    #[test]
    fn dynamic_view_hashed_path_properties() {
        let empty = HashedPath::new("");
        assert!(empty.is_empty());
        assert_eq!(empty.len(), 0);

        let single = HashedPath::new("field");
        assert!(!single.is_empty());
        assert_eq!(single.len(), 1);

        let complex = HashedPath::new("a.b[0].c");
        assert_eq!(complex.len(), 4); // a, b, 0, c
    }

    // ══════════════════════════════════════════════════════════════════════
    // DynamicValue PartialEq tests
    // ══════════════════════════════════════════════════════════════════════

    #[test]
    fn dynamic_value_eq_same_variant() {
        assert_eq!(DynamicValue::Void, DynamicValue::Void);
        assert_eq!(DynamicValue::Bool(true), DynamicValue::Bool(true));
        assert_ne!(DynamicValue::Bool(true), DynamicValue::Bool(false));
        assert_eq!(DynamicValue::U32(42), DynamicValue::U32(42));
        assert_ne!(DynamicValue::U32(42), DynamicValue::U32(99));
        assert_eq!(DynamicValue::I64(-1), DynamicValue::I64(-1));
        assert_eq!(DynamicValue::F64(3.14), DynamicValue::F64(3.14));
        assert_eq!(DynamicValue::Text("hello"), DynamicValue::Text("hello"));
        assert_ne!(DynamicValue::Text("hello"), DynamicValue::Text("world"));
    }

    #[test]
    fn dynamic_value_eq_cross_variant_numeric() {
        // Same value, different types
        assert_eq!(DynamicValue::U32(5), DynamicValue::U64(5));
        assert_eq!(DynamicValue::I32(5), DynamicValue::U64(5));
        assert_eq!(DynamicValue::U8(255), DynamicValue::U16(255));
        assert_eq!(DynamicValue::I64(100), DynamicValue::U32(100));

        // Negative signed != unsigned
        assert_ne!(DynamicValue::I32(-1), DynamicValue::U32(0xFFFF_FFFF));

        // Float vs int
        assert_eq!(DynamicValue::F64(42.0), DynamicValue::U32(42));
        assert_eq!(DynamicValue::F32(5.0), DynamicValue::I32(5));

        // Bool vs int
        assert_eq!(DynamicValue::Bool(true), DynamicValue::U8(1));
        assert_eq!(DynamicValue::Bool(false), DynamicValue::U32(0));
    }

    #[test]
    fn dynamic_value_eq_void_not_equal_to_others() {
        assert_ne!(DynamicValue::Void, DynamicValue::U32(0));
        assert_ne!(DynamicValue::Void, DynamicValue::Bool(false));
        assert_ne!(DynamicValue::Void, DynamicValue::Text(""));
    }

    // ══════════════════════════════════════════════════════════════════════
    // DynamicValue PartialOrd tests
    // ══════════════════════════════════════════════════════════════════════

    #[test]
    fn dynamic_value_ord_same_variant() {
        assert!(DynamicValue::U32(1) < DynamicValue::U32(2));
        assert!(DynamicValue::I32(-1) < DynamicValue::I32(0));
        assert!(DynamicValue::F64(1.0) < DynamicValue::F64(2.0));
        assert!(DynamicValue::Text("abc") < DynamicValue::Text("abd"));
        assert!(DynamicValue::Text("abc") < DynamicValue::Text("abcd"));
    }

    #[test]
    fn dynamic_value_ord_cross_variant_numeric() {
        // Signed negative < unsigned
        assert!(DynamicValue::I32(-1) < DynamicValue::U32(0));
        assert!(DynamicValue::I64(-100) < DynamicValue::U8(0));

        // Same value across types
        assert_eq!(
            DynamicValue::U32(5).partial_cmp(&DynamicValue::I64(5)),
            Some(std::cmp::Ordering::Equal)
        );

        // Float vs int ordering
        assert!(DynamicValue::F64(2.5) < DynamicValue::U32(3));
        assert!(DynamicValue::U32(3) > DynamicValue::F64(2.5));
    }

    #[test]
    fn dynamic_value_ord_incomparable() {
        // Void vs numeric: not comparable
        assert_eq!(DynamicValue::Void.partial_cmp(&DynamicValue::U32(0)), None);
        // Text vs numeric: not comparable
        assert_eq!(DynamicValue::Text("5").partial_cmp(&DynamicValue::U32(5)), None);
    }

    #[test]
    fn dynamic_value_ord_nan() {
        // NaN is not equal to itself
        assert_ne!(DynamicValue::F64(f64::NAN), DynamicValue::F64(f64::NAN));
        assert_eq!(
            DynamicValue::F64(f64::NAN).partial_cmp(&DynamicValue::F64(f64::NAN)),
            None
        );
    }

    // ══════════════════════════════════════════════════════════════════════
    // CompiledPath tests
    // ══════════════════════════════════════════════════════════════════════

    #[test]
    fn compiled_path_basic_field() {
        let schema = wit_schema(&[
            ("x", DynamicType::U32),
            ("y", DynamicType::U32),
        ]);

        let cp_x = CompiledPath::compile("x", &schema).unwrap();
        let cp_y = CompiledPath::compile("y", &schema).unwrap();
        assert_eq!(cp_x.len(), 1);
        assert_eq!(cp_y.len(), 1);

        let buf = (42u32, 99u32).wit_pack();
        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);

        assert_eq!(view.eval_compiled(&cp_x).as_u32(), Some(42));
        assert_eq!(view.eval_compiled(&cp_y).as_u32(), Some(99));
    }

    #[test]
    fn compiled_path_nonexistent_field() {
        let schema = wit_schema(&[("x", DynamicType::U32)]);
        assert!(CompiledPath::compile("nonexistent", &schema).is_none());
    }

    #[test]
    fn compiled_path_nested_struct() {
        // Inner: struct { val: u32 }
        let inner_locs_data: Vec<(u32, u32)> = vec![(4, 4)];
        let (inner_locs, inner_total, _) = compute_struct_layout(&inner_locs_data);

        let inner_schema = SchemaBuilder::new()
            .field_scalar("val", DynamicType::U32, inner_locs[0].offset)
            .data_words(1)
            .build();

        // Outer: struct { id: u32, inner: Inner }
        let outer_locs_data: Vec<(u32, u32)> = vec![(4, 4), (4, inner_total)];
        let (outer_locs, _outer_total, _) = compute_struct_layout(&outer_locs_data);

        let mut inner_fd = FieldDesc::scalar("inner", DynamicType::Struct, outer_locs[1].offset);
        inner_fd.nested = Some(Box::new(inner_schema));
        inner_fd.byte_size = inner_total as u8;

        let outer_schema = SchemaBuilder::new()
            .field_scalar("id", DynamicType::U32, outer_locs[0].offset)
            .field(inner_fd)
            .data_words(1)
            .build();

        let buf = (1u32, 42u32).wit_pack();
        let view = DynamicView::new(&buf, &outer_schema, WireFormat::Wit);

        let cp = CompiledPath::compile("inner.val", &outer_schema).unwrap();
        assert_eq!(cp.len(), 2);
        assert_eq!(view.eval_compiled(&cp).as_u32(), Some(42));
    }

    #[test]
    fn compiled_path_matches_hashed_path() {
        // Verify that eval (HashedPath) and eval_compiled (CompiledPath)
        // produce identical results for all fields.
        let schema = wit_schema(&[
            ("a", DynamicType::U32),
            ("b", DynamicType::I64),
            ("c", DynamicType::F64),
        ]);

        let buf = (42u32, -7i64, 3.14f64).wit_pack();
        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);

        for field_name in &["a", "b", "c"] {
            let hp = HashedPath::new(field_name);
            let cp = CompiledPath::compile(field_name, &schema).unwrap();

            let val_hashed = view.eval(&hp);
            let val_compiled = view.eval_compiled(&cp);

            // Both should produce the same result
            assert_eq!(val_hashed, val_compiled,
                "mismatch for field '{}': hashed={:?} compiled={:?}",
                field_name, val_hashed, val_compiled);
        }

        // Also test non-existent field
        let hp_missing = HashedPath::new("nonexistent");
        let val_missing = view.eval(&hp_missing);
        assert!(!val_missing.exists());
    }

    #[test]
    fn compiled_path_deep_matches_hashed() {
        // Inner: struct { x: i32, y: i32 }
        let inner_fields: Vec<(u32, u32)> = vec![(4, 4), (4, 4)];
        let (inner_locs, inner_total, _) = compute_struct_layout(&inner_fields);

        let inner_schema = SchemaBuilder::new()
            .field_scalar("x", DynamicType::I32, inner_locs[0].offset)
            .field_scalar("y", DynamicType::I32, inner_locs[1].offset)
            .data_words(1)
            .build();

        // Outer: struct { pos: Inner }
        let outer_fields: Vec<(u32, u32)> = vec![(4, inner_total)];
        let (outer_locs, _outer_total, _) = compute_struct_layout(&outer_fields);

        let mut pos_fd = FieldDesc::scalar("pos", DynamicType::Struct, outer_locs[0].offset);
        pos_fd.nested = Some(Box::new(inner_schema));
        pos_fd.byte_size = inner_total as u8;

        let outer_schema = SchemaBuilder::new()
            .field(pos_fd)
            .data_words(1)
            .build();

        let buf = (10i32, 20i32).wit_pack();
        let view = DynamicView::new(&buf, &outer_schema, WireFormat::Wit);

        for path_str in &["pos.x", "pos.y"] {
            let hp = HashedPath::new(path_str);
            let cp = CompiledPath::compile(path_str, &outer_schema).unwrap();

            let val_hashed = view.eval(&hp);
            let val_compiled = view.eval_compiled(&cp);
            assert_eq!(val_hashed, val_compiled,
                "mismatch for path '{}': hashed={:?} compiled={:?}",
                path_str, val_hashed, val_compiled);
        }
    }

    #[test]
    fn compiled_path_with_array_index() {
        let mut fd = FieldDesc::scalar("items", DynamicType::Vector, 0);
        fd.is_ptr = false;
        fd.byte_size = 4; // u32 elements
        fd.nested = None;

        let schema = SchemaBuilder::new().field(fd).data_words(1).build();

        let items: Vec<u32> = vec![100, 200, 300];
        let buf = items.wit_pack();

        let view = DynamicView::new(&buf, &schema, WireFormat::Wit);

        let cp = CompiledPath::compile("items", &schema).unwrap();
        // Can't compile items[0] since the index part doesn't require schema resolution,
        // but we can still eval it via the compiled path for the field + manual get()
        let list_val = view.eval_compiled(&cp);
        assert_eq!(list_val.get(0).as_u32(), Some(100));
        assert_eq!(list_val.get(1).as_u32(), Some(200));
        assert_eq!(list_val.get(2).as_u32(), Some(300));
    }

    #[test]
    fn compiled_path_empty() {
        let schema = wit_schema(&[("x", DynamicType::U32)]);
        let cp = CompiledPath::compile("", &schema).unwrap();
        assert!(cp.is_empty());
    }
}
