//! Derive macro for `ToDynamicSchema`.
//!
//! Generates a `DynamicSchema` from a Rust struct's fields by mapping field
//! names and types to `FieldDesc` entries. Uses WIT-style natural alignment
//! for computing field offsets.

use proc_macro::TokenStream;
use proc_macro2::TokenStream as TokenStream2;
use quote::quote;
use syn::{parse_macro_input, Data, DeriveInput, Fields, Type};

/// Map a Rust type to the corresponding `DynamicType` variant and byte size.
fn type_to_dynamic(ty: &Type) -> Option<(TokenStream2, u32, u32)> {
    let path = match ty {
        Type::Path(p) => p,
        _ => return None,
    };
    let seg = path.path.segments.last()?;
    let ident = seg.ident.to_string();

    // Returns (DynamicType variant, alignment, size)
    let result = match ident.as_str() {
        "bool" => (quote! { Bool }, 1, 1),
        "u8" => (quote! { U8 }, 1, 1),
        "i8" => (quote! { I8 }, 1, 1),
        "u16" => (quote! { U16 }, 2, 2),
        "i16" => (quote! { I16 }, 2, 2),
        "u32" => (quote! { U32 }, 4, 4),
        "i32" => (quote! { I32 }, 4, 4),
        "f32" => (quote! { F32 }, 4, 4),
        "u64" => (quote! { U64 }, 8, 8),
        "i64" => (quote! { I64 }, 8, 8),
        "f64" => (quote! { F64 }, 8, 8),
        "String" => (quote! { Text }, 4, 8),
        _ => return None,
    };
    Some(result)
}

pub fn dynamic_schema_derive_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    let name = &input.ident;

    let fields = match &input.data {
        Data::Struct(data) => match &data.fields {
            Fields::Named(named) => &named.named,
            _ => {
                return syn::Error::new_spanned(
                    &input.ident,
                    "ToDynamicSchema only supports structs with named fields",
                )
                .to_compile_error()
                .into();
            }
        },
        _ => {
            return syn::Error::new_spanned(
                &input.ident,
                "ToDynamicSchema only supports structs",
            )
            .to_compile_error()
            .into();
        }
    };

    // Compute offsets using natural alignment (WIT-style).
    // We do this at compile time in the macro.
    let mut field_infos: Vec<(String, TokenStream2, u32, u32)> = Vec::new();
    for field in fields.iter() {
        let field_name = field.ident.as_ref().unwrap().to_string();
        match type_to_dynamic(&field.ty) {
            Some((dt, align, size)) => {
                field_infos.push((field_name, dt, align, size));
            }
            None => {
                return syn::Error::new_spanned(
                    &field.ty,
                    format!(
                        "ToDynamicSchema: unsupported type for field '{}'",
                        field_name
                    ),
                )
                .to_compile_error()
                .into();
            }
        }
    }

    // Compute offsets at macro expansion time
    let mut offset: u32 = 0;
    let mut max_align: u32 = 1;
    let mut field_adds: Vec<TokenStream2> = Vec::new();

    for (fname, dt, align, size) in &field_infos {
        // Align offset
        let misalign = offset % align;
        if misalign != 0 {
            offset += align - misalign;
        }
        if *align > max_align {
            max_align = *align;
        }

        let field_offset = offset;
        let byte_size = *size as u8;

        field_adds.push(quote! {
            builder = builder.field({
                let mut fd = psio1::dynamic_schema::FieldDesc::scalar(
                    #fname,
                    psio1::dynamic_schema::DynamicType::#dt,
                    #field_offset,
                );
                fd.byte_size = #byte_size;
                fd
            });
        });

        offset += size;
    }

    // Compute data_words (ceiling division by 8)
    let total_misalign = offset % max_align;
    if total_misalign != 0 {
        offset += max_align - total_misalign;
    }
    let data_words = ((offset + 7) / 8) as u16;

    let expanded = quote! {
        impl psio1::dynamic_view::ToDynamicSchema for #name {
            fn dynamic_schema() -> psio1::dynamic_schema::DynamicSchema {
                let mut builder = psio1::dynamic_schema::SchemaBuilder::new();
                #(#field_adds)*
                builder.data_words(#data_words).build()
            }
        }
    };

    expanded.into()
}
