//! FlatBuffer schema export — generate .fbs IDL text from type metadata.
//!
//! This module provides the `FbSchema` trait for types that can describe
//! their FlatBuffer schema, and a function to render that schema as .fbs text.
//!
//! Without derive macros, schemas are built manually. Once derive macros are
//! added, `FbSchema` will be auto-generated from struct definitions.

use std::fmt::Write;

/// Describes a single field in a FlatBuffer table schema.
#[derive(Debug, Clone)]
pub struct FbFieldSchema {
    /// Field name.
    pub name: String,
    /// FlatBuffer type name (e.g., "uint32", "string", "[int16]").
    pub fb_type: String,
    /// Optional default value as a string (e.g., "0", "\"\"").
    pub default: Option<String>,
}

/// Describes a FlatBuffer table or struct type.
#[derive(Debug, Clone)]
pub struct FbTypeSchema {
    /// Type name (e.g., "Token").
    pub name: String,
    /// Whether this is a struct (all-fixed-size-scalars) vs table.
    pub is_struct: bool,
    /// Fields in declaration order.
    pub fields: Vec<FbFieldSchema>,
}

/// Trait for types that can export their FlatBuffer schema.
pub trait FbSchema {
    /// Returns the schema description for this type.
    fn fb_schema() -> FbTypeSchema;
}

/// Maps a Rust type name to a FlatBuffer type name.
pub fn rust_to_fbs_type(rust_type: &str) -> &str {
    match rust_type {
        "bool" => "bool",
        "u8" => "ubyte",
        "i8" => "byte",
        "u16" => "ushort",
        "i16" => "short",
        "u32" => "uint",
        "i32" => "int",
        "u64" => "ulong",
        "i64" => "long",
        "f32" => "float",
        "f64" => "double",
        "String" | "&str" => "string",
        other => other,
    }
}

/// Generate .fbs IDL text from a set of type schemas.
pub fn to_fbs_text(schemas: &[FbTypeSchema]) -> String {
    let mut out = String::new();
    for (i, schema) in schemas.iter().enumerate() {
        if i > 0 {
            out.push('\n');
        }
        let kind = if schema.is_struct { "struct" } else { "table" };
        writeln!(out, "{} {} {{", kind, schema.name).unwrap();
        for field in &schema.fields {
            write!(out, "  {}:{}", field.name, field.fb_type).unwrap();
            if let Some(ref def) = field.default {
                write!(out, " = {}", def).unwrap();
            }
            writeln!(out, ";").unwrap();
        }
        writeln!(out, "}}").unwrap();
    }
    out
}

/// Generate a complete .fbs schema for a single type.
pub fn to_fbs_schema<T: FbSchema>() -> String {
    let schema = T::fb_schema();
    to_fbs_text(&[schema])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn flatbuf_schema_type_mapping() {
        assert_eq!(rust_to_fbs_type("u32"), "uint");
        assert_eq!(rust_to_fbs_type("i64"), "long");
        assert_eq!(rust_to_fbs_type("String"), "string");
        assert_eq!(rust_to_fbs_type("f32"), "float");
        assert_eq!(rust_to_fbs_type("bool"), "bool");
        assert_eq!(rust_to_fbs_type("MyStruct"), "MyStruct");
    }

    #[test]
    fn flatbuf_schema_generation() {
        let schema = FbTypeSchema {
            name: "Token".to_string(),
            is_struct: false,
            fields: vec![
                FbFieldSchema {
                    name: "kind".to_string(),
                    fb_type: "ushort".to_string(),
                    default: Some("0".to_string()),
                },
                FbFieldSchema {
                    name: "text".to_string(),
                    fb_type: "string".to_string(),
                    default: None,
                },
            ],
        };

        let text = to_fbs_text(&[schema]);
        assert!(text.contains("table Token {"));
        assert!(text.contains("  kind:ushort = 0;"));
        assert!(text.contains("  text:string;"));
        assert!(text.contains("}"));
    }

    #[test]
    fn flatbuf_struct_schema() {
        let schema = FbTypeSchema {
            name: "Vec3".to_string(),
            is_struct: true,
            fields: vec![
                FbFieldSchema {
                    name: "x".to_string(),
                    fb_type: "float".to_string(),
                    default: None,
                },
                FbFieldSchema {
                    name: "y".to_string(),
                    fb_type: "float".to_string(),
                    default: None,
                },
                FbFieldSchema {
                    name: "z".to_string(),
                    fb_type: "float".to_string(),
                    default: None,
                },
            ],
        };

        let text = to_fbs_text(&[schema]);
        assert!(text.contains("struct Vec3 {"));
    }
}
