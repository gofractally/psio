//! Cap'n Proto schema generation.
//!
//! Generates `.capnp` IDL text from Rust type metadata.

use crate::capnp::layout::FieldKind;

/// Describes a field for schema generation.
#[derive(Debug, Clone)]
pub struct SchemaField {
    /// Field name.
    pub name: String,
    /// Cap'n Proto type name (e.g., "UInt32", "Text", "List(UInt8)").
    pub capnp_type: String,
    /// Ordinal (field number in the schema).
    pub ordinal: u16,
}

/// Describes a struct for schema generation.
#[derive(Debug, Clone)]
pub struct SchemaStruct {
    /// Struct name.
    pub name: String,
    /// Fields in declaration order.
    pub fields: Vec<SchemaField>,
}

/// Trait for types that can export their Cap'n Proto schema.
pub trait CapnpSchema {
    /// Return the struct schema description.
    fn capnp_schema() -> SchemaStruct;
}

/// Generate a `.capnp` IDL string from a `SchemaStruct`.
pub fn to_capnp_schema(schema: &SchemaStruct) -> String {
    let mut out = String::new();
    out.push_str(&format!("struct {} {{\n", schema.name));
    for field in &schema.fields {
        out.push_str(&format!(
            "  {} @{} :{};",
            field.name, field.ordinal, field.capnp_type
        ));
        out.push('\n');
    }
    out.push_str("}\n");
    out
}

/// Map a `FieldKind` to its Cap'n Proto type name.
pub fn capnp_type_name(kind: &FieldKind, rust_type_hint: &str) -> String {
    match kind {
        FieldKind::Void => "Void".to_string(),
        FieldKind::Bool => "Bool".to_string(),
        FieldKind::Scalar(1) => {
            if rust_type_hint.contains("i8") {
                "Int8".to_string()
            } else {
                "UInt8".to_string()
            }
        }
        FieldKind::Scalar(2) => {
            if rust_type_hint.contains("i16") {
                "Int16".to_string()
            } else {
                "UInt16".to_string()
            }
        }
        FieldKind::Scalar(4) => {
            if rust_type_hint.contains("i32") {
                "Int32".to_string()
            } else if rust_type_hint.contains("f32") {
                "Float32".to_string()
            } else {
                "UInt32".to_string()
            }
        }
        FieldKind::Scalar(8) => {
            if rust_type_hint.contains("i64") {
                "Int64".to_string()
            } else if rust_type_hint.contains("f64") {
                "Float64".to_string()
            } else {
                "UInt64".to_string()
            }
        }
        FieldKind::Scalar(_) => "Data".to_string(),
        FieldKind::Pointer => {
            // For pointers, rust_type_hint distinguishes String vs Vec vs struct
            if rust_type_hint == "String" || rust_type_hint == "str" {
                "Text".to_string()
            } else if rust_type_hint.starts_with("Vec<") {
                let inner = &rust_type_hint[4..rust_type_hint.len() - 1];
                format!("List({})", capnp_type_from_rust(inner))
            } else {
                // Nested struct
                rust_type_hint.to_string()
            }
        }
    }
}

/// Convert a Rust type name to a Cap'n Proto type name.
fn capnp_type_from_rust(rust_type: &str) -> String {
    match rust_type {
        "bool" => "Bool".to_string(),
        "u8" => "UInt8".to_string(),
        "i8" => "Int8".to_string(),
        "u16" => "UInt16".to_string(),
        "i16" => "Int16".to_string(),
        "u32" => "UInt32".to_string(),
        "i32" => "Int32".to_string(),
        "u64" => "UInt64".to_string(),
        "i64" => "Int64".to_string(),
        "f32" => "Float32".to_string(),
        "f64" => "Float64".to_string(),
        "String" | "str" => "Text".to_string(),
        other => other.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_schema_generation() {
        let schema = SchemaStruct {
            name: "Order".to_string(),
            fields: vec![
                SchemaField {
                    name: "id".to_string(),
                    capnp_type: "UInt64".to_string(),
                    ordinal: 0,
                },
                SchemaField {
                    name: "name".to_string(),
                    capnp_type: "Text".to_string(),
                    ordinal: 1,
                },
                SchemaField {
                    name: "amount".to_string(),
                    capnp_type: "Float64".to_string(),
                    ordinal: 2,
                },
            ],
        };

        let idl = to_capnp_schema(&schema);
        assert!(idl.contains("struct Order {"));
        assert!(idl.contains("id @0 :UInt64;"));
        assert!(idl.contains("name @1 :Text;"));
        assert!(idl.contains("amount @2 :Float64;"));
    }

    #[test]
    fn test_type_name_mapping() {
        assert_eq!(capnp_type_name(&FieldKind::Bool, "bool"), "Bool");
        assert_eq!(capnp_type_name(&FieldKind::Scalar(4), "u32"), "UInt32");
        assert_eq!(capnp_type_name(&FieldKind::Scalar(4), "i32"), "Int32");
        assert_eq!(capnp_type_name(&FieldKind::Scalar(4), "f32"), "Float32");
        assert_eq!(capnp_type_name(&FieldKind::Pointer, "String"), "Text");
        assert_eq!(
            capnp_type_name(&FieldKind::Pointer, "Vec<u32>"),
            "List(UInt32)"
        );
    }
}
