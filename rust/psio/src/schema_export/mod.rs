//! Schema export: generate .capnp, .fbs, and .wit IDL from Rust types.
//!
//! Re-exports the per-format schema traits and functions, plus a unified
//! WIT export path from `DynamicSchema`.

pub use crate::capnp::schema::{to_capnp_schema, CapnpSchema};
pub use crate::flatbuf::schema::{to_fbs_schema, FbSchema, to_fbs_text};

use crate::dynamic_schema::{DynamicSchema, DynamicType};

/// Map a `DynamicType` to a WIT type name.
pub fn dynamic_type_to_wit(ty: DynamicType) -> &'static str {
    match ty {
        DynamicType::Bool => "bool",
        DynamicType::I8 => "s8",
        DynamicType::I16 => "s16",
        DynamicType::I32 => "s32",
        DynamicType::I64 => "s64",
        DynamicType::U8 => "u8",
        DynamicType::U16 => "u16",
        DynamicType::U32 => "u32",
        DynamicType::U64 => "u64",
        DynamicType::F32 => "f32",
        DynamicType::F64 => "f64",
        DynamicType::Text => "string",
        DynamicType::Data => "list<u8>",
        DynamicType::Vector => "list<u8>",
        DynamicType::Void => "()",
        DynamicType::Struct => "/* struct */",
        DynamicType::Variant => "/* variant */",
    }
}

/// Convert a camelCase or PascalCase or snake_case name to kebab-case for WIT.
fn to_kebab_case(s: &str) -> String {
    let mut out = String::new();
    for (i, ch) in s.chars().enumerate() {
        if ch == '_' {
            out.push('-');
        } else if ch.is_uppercase() && i > 0 {
            out.push('-');
            out.push(ch.to_lowercase().next().unwrap());
        } else {
            out.push(ch.to_lowercase().next().unwrap());
        }
    }
    out
}

/// Generate `.wit` IDL text from a `DynamicSchema`.
///
/// Produces a WIT `record` definition with fields in declaration order,
/// mapping `DynamicType` variants to WIT type names.
///
/// # Example
///
/// ```
/// use psio1::dynamic_schema::{SchemaBuilder, DynamicType};
/// use psio1::schema_export::to_wit_schema;
///
/// let schema = SchemaBuilder::new()
///     .field_scalar("x", DynamicType::I32, 0)
///     .field_scalar("y", DynamicType::I32, 4)
///     .data_words(1)
///     .build();
///
/// let wit = to_wit_schema("Point", &schema);
/// assert!(wit.contains("record point {"));
/// assert!(wit.contains("x: s32,"));
/// assert!(wit.contains("y: s32,"));
/// ```
pub fn to_wit_schema(name: &str, schema: &DynamicSchema) -> String {
    let mut out = String::new();
    out.push_str(&format!("record {} {{\n", to_kebab_case(name)));
    for field in schema.fields_ordered() {
        let wit_type = dynamic_type_to_wit(field.ty);
        out.push_str(&format!(
            "  {}: {},\n",
            to_kebab_case(&field.name),
            wit_type
        ));
    }
    out.push_str("}\n");
    out
}

/// Map a `DynamicType` to a Cap'n Proto type name.
pub fn dynamic_type_to_capnp(ty: DynamicType) -> &'static str {
    match ty {
        DynamicType::Bool => "Bool",
        DynamicType::I8 => "Int8",
        DynamicType::I16 => "Int16",
        DynamicType::I32 => "Int32",
        DynamicType::I64 => "Int64",
        DynamicType::U8 => "UInt8",
        DynamicType::U16 => "UInt16",
        DynamicType::U32 => "UInt32",
        DynamicType::U64 => "UInt64",
        DynamicType::F32 => "Float32",
        DynamicType::F64 => "Float64",
        DynamicType::Text => "Text",
        DynamicType::Data => "Data",
        _ => "Void",
    }
}

/// Generate `.capnp` IDL text from a `DynamicSchema`.
pub fn to_capnp_schema_from_dynamic(name: &str, schema: &DynamicSchema) -> String {
    let mut out = String::new();
    out.push_str(&format!("struct {} {{\n", name));
    for (i, field) in schema.fields_ordered().enumerate() {
        out.push_str(&format!(
            "  {} @{} :{};\n",
            field.name,
            i,
            dynamic_type_to_capnp(field.ty)
        ));
    }
    out.push_str("}\n");
    out
}

/// Map a `DynamicType` to a FlatBuffers type name.
pub fn dynamic_type_to_fbs(ty: DynamicType) -> &'static str {
    match ty {
        DynamicType::Bool => "bool",
        DynamicType::I8 => "byte",
        DynamicType::I16 => "short",
        DynamicType::I32 => "int",
        DynamicType::I64 => "long",
        DynamicType::U8 => "ubyte",
        DynamicType::U16 => "ushort",
        DynamicType::U32 => "uint",
        DynamicType::U64 => "ulong",
        DynamicType::F32 => "float",
        DynamicType::F64 => "double",
        DynamicType::Text => "string",
        DynamicType::Data => "[ubyte]",
        _ => "void",
    }
}

/// Generate `.fbs` IDL text from a `DynamicSchema`.
pub fn to_fbs_schema_from_dynamic(name: &str, schema: &DynamicSchema) -> String {
    let mut out = String::new();
    out.push_str(&format!("table {} {{\n", name));
    for field in schema.fields_ordered() {
        out.push_str(&format!(
            "  {}:{};\n",
            field.name,
            dynamic_type_to_fbs(field.ty)
        ));
    }
    out.push_str("}\n");
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dynamic_schema::{SchemaBuilder, DynamicType};

    fn point_schema() -> DynamicSchema {
        SchemaBuilder::new()
            .field_scalar("x", DynamicType::F32, 0)
            .field_scalar("y", DynamicType::F32, 4)
            .data_words(1)
            .build()
    }

    #[test]
    fn wit_schema_point() {
        let schema = point_schema();
        let wit = to_wit_schema("Point", &schema);
        assert!(wit.contains("record point {"));
        assert!(wit.contains("  x: f32,"));
        assert!(wit.contains("  y: f32,"));
        assert!(wit.ends_with("}\n"));
    }

    #[test]
    fn wit_schema_all_types() {
        let schema = SchemaBuilder::new()
            .field_scalar("flag", DynamicType::Bool, 0)
            .field_scalar("byte_val", DynamicType::U8, 1)
            .field_scalar("signed_byte", DynamicType::I8, 2)
            .field_scalar("short_val", DynamicType::U16, 4)
            .field_scalar("signed_short", DynamicType::I16, 6)
            .field_scalar("int_val", DynamicType::U32, 8)
            .field_scalar("signed_int", DynamicType::I32, 12)
            .field_scalar("long_val", DynamicType::U64, 16)
            .field_scalar("signed_long", DynamicType::I64, 24)
            .field_scalar("float_val", DynamicType::F32, 32)
            .field_scalar("double_val", DynamicType::F64, 40)
            .field_pointer("text_val", DynamicType::Text, 0)
            .data_words(6)
            .ptr_count(1)
            .build();

        let wit = to_wit_schema("AllTypes", &schema);
        assert!(wit.contains("record all-types {"));
        assert!(wit.contains("  flag: bool,"));
        assert!(wit.contains("  byte-val: u8,"));
        assert!(wit.contains("  signed-byte: s8,"));
        assert!(wit.contains("  short-val: u16,"));
        assert!(wit.contains("  signed-short: s16,"));
        assert!(wit.contains("  int-val: u32,"));
        assert!(wit.contains("  signed-int: s32,"));
        assert!(wit.contains("  long-val: u64,"));
        assert!(wit.contains("  signed-long: s64,"));
        assert!(wit.contains("  float-val: f32,"));
        assert!(wit.contains("  double-val: f64,"));
        assert!(wit.contains("  text-val: string,"));
    }

    #[test]
    fn capnp_schema_from_dynamic() {
        let schema = point_schema();
        let capnp = to_capnp_schema_from_dynamic("Point", &schema);
        assert!(capnp.contains("struct Point {"));
        assert!(capnp.contains("  x @0 :Float32;"));
        assert!(capnp.contains("  y @1 :Float32;"));
    }

    #[test]
    fn fbs_schema_from_dynamic() {
        let schema = point_schema();
        let fbs = to_fbs_schema_from_dynamic("Point", &schema);
        assert!(fbs.contains("table Point {"));
        assert!(fbs.contains("  x:float;"));
        assert!(fbs.contains("  y:float;"));
    }

    #[test]
    fn kebab_case_conversion() {
        assert_eq!(to_kebab_case("Point"), "point");
        assert_eq!(to_kebab_case("UserProfile"), "user-profile");
        assert_eq!(to_kebab_case("sensor_reading"), "sensor-reading");
        assert_eq!(to_kebab_case("device_id"), "device-id");
        assert_eq!(to_kebab_case("abc"), "abc");
    }
}
