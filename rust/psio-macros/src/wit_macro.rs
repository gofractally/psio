//! Derive macros for WIT Canonical ABI serialization (WitPack, WitUnpack).
//!
//! Generates `impl WitLayout`, `impl WitPack`, and `impl WitUnpack` for named structs,
//! computing field offsets from WIT alignment rules and emitting the appropriate
//! pack/unpack code.

use darling::FromDeriveInput;
use proc_macro::TokenStream;
use quote::quote;
use std::str::FromStr;
use syn::{parse_macro_input, Data, DataEnum, DeriveInput, Fields, Type};

use crate::fracpack_macro::Options;

// ── Type classification ─────────────────────────────────────────────────

enum WitFieldClass {
    Bool,
    U8,
    I8,
    U16,
    I16,
    U32,
    I32,
    F32,
    U64,
    I64,
    F64,
    Text,
    Vec,     // Vec<T> for any T
    Struct,  // Nested struct
}

fn classify_type(ty: &Type) -> WitFieldClass {
    let ty_str = quote!(#ty).to_string().replace(' ', "");
    match ty_str.as_str() {
        "bool" => WitFieldClass::Bool,
        "u8" => WitFieldClass::U8,
        "i8" => WitFieldClass::I8,
        "u16" => WitFieldClass::U16,
        "i16" => WitFieldClass::I16,
        "u32" => WitFieldClass::U32,
        "i32" => WitFieldClass::I32,
        "f32" => WitFieldClass::F32,
        "u64" => WitFieldClass::U64,
        "i64" => WitFieldClass::I64,
        "f64" => WitFieldClass::F64,
        "String" => WitFieldClass::Text,
        _ => {
            if let Type::Path(tp) = ty {
                let seg = tp.path.segments.last().unwrap();
                if seg.ident == "Vec" {
                    return WitFieldClass::Vec;
                }
            }
            WitFieldClass::Struct
        }
    }
}

/// Generate the wit_store call for a field.
fn store_field_tokens(
    cls: &WitFieldClass,
    mod_path: &proc_macro2::TokenStream,
    field_expr: &proc_macro2::TokenStream,
    field_ty: &Type,
) -> proc_macro2::TokenStream {
    match cls {
        WitFieldClass::Bool => {
            quote! { packer.store_u8(dest + loc.offset, if #field_expr { 1 } else { 0 }); }
        }
        WitFieldClass::U8 => quote! { packer.store_u8(dest + loc.offset, #field_expr); },
        WitFieldClass::I8 => quote! { packer.store_u8(dest + loc.offset, #field_expr as u8); },
        WitFieldClass::U16 => quote! { packer.store_u16(dest + loc.offset, #field_expr); },
        WitFieldClass::I16 => quote! { packer.store_u16(dest + loc.offset, #field_expr as u16); },
        WitFieldClass::U32 => quote! { packer.store_u32(dest + loc.offset, #field_expr); },
        WitFieldClass::I32 => quote! { packer.store_u32(dest + loc.offset, #field_expr as u32); },
        WitFieldClass::F32 => quote! { packer.store_f32(dest + loc.offset, #field_expr); },
        WitFieldClass::U64 => quote! { packer.store_u64(dest + loc.offset, #field_expr); },
        WitFieldClass::I64 => quote! { packer.store_u64(dest + loc.offset, #field_expr as u64); },
        WitFieldClass::F64 => quote! { packer.store_f64(dest + loc.offset, #field_expr); },
        WitFieldClass::Text | WitFieldClass::Vec | WitFieldClass::Struct => {
            // Delegate to the type's own WitPack::wit_store
            quote! {
                <#field_ty as #mod_path::wit::pack::WitPack>::wit_store(&#field_expr, packer, dest + loc.offset);
            }
        }
    }
}

/// Generate the accumulate_size call for a field.
fn accumulate_tokens(
    cls: &WitFieldClass,
    mod_path: &proc_macro2::TokenStream,
    field_expr: &proc_macro2::TokenStream,
    field_ty: &Type,
) -> proc_macro2::TokenStream {
    match cls {
        // Scalars have no variable-length data
        WitFieldClass::Bool | WitFieldClass::U8 | WitFieldClass::I8 |
        WitFieldClass::U16 | WitFieldClass::I16 |
        WitFieldClass::U32 | WitFieldClass::I32 | WitFieldClass::F32 |
        WitFieldClass::U64 | WitFieldClass::I64 | WitFieldClass::F64 => {
            quote! {} // no-op
        }
        WitFieldClass::Text | WitFieldClass::Vec | WitFieldClass::Struct => {
            quote! {
                <#field_ty as #mod_path::wit::pack::WitPack>::wit_accumulate_size(&#field_expr, bump);
            }
        }
    }
}

/// Generate the unpack expression for a field.
fn unpack_field_tokens(
    cls: &WitFieldClass,
    mod_path: &proc_macro2::TokenStream,
    field_ty: &Type,
) -> proc_macro2::TokenStream {
    match cls {
        WitFieldClass::Bool => quote! { sub.read_bool(0) },
        WitFieldClass::U8 => quote! { sub.read_u8(0) },
        WitFieldClass::I8 => quote! { sub.read_i8(0) },
        WitFieldClass::U16 => quote! { sub.read_u16(0) },
        WitFieldClass::I16 => quote! { sub.read_i16(0) },
        WitFieldClass::U32 => quote! { sub.read_u32(0) },
        WitFieldClass::I32 => quote! { sub.read_i32(0) },
        WitFieldClass::F32 => quote! { sub.read_f32(0) },
        WitFieldClass::U64 => quote! { sub.read_u64(0) },
        WitFieldClass::I64 => quote! { sub.read_i64(0) },
        WitFieldClass::F64 => quote! { sub.read_f64(0) },
        WitFieldClass::Text | WitFieldClass::Vec | WitFieldClass::Struct => {
            quote! {
                <#field_ty as #mod_path::wit::view::WitUnpack>::wit_unpack_from(&sub)?
            }
        }
    }
}

// ── Entry points ────────────────────────────────────────────────────────

pub fn wit_pack_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    let opts = match Options::from_derive_input(&input) {
        Ok(val) => val,
        Err(err) => return err.write_errors().into(),
    };
    let mod_path = proc_macro2::TokenStream::from_str(&opts.fracpack_mod).unwrap();

    match &input.data {
        Data::Struct(data) => {
            if let Fields::Named(named) = &data.fields {
                gen_wit_pack(&mod_path, &input, named)
            } else {
                panic!("WitPack only supports named structs");
            }
        }
        Data::Enum(data) => gen_wit_pack_enum(&mod_path, &input, data),
        _ => panic!("WitPack only supports structs and enums"),
    }
}

pub fn wit_unpack_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    let opts = match Options::from_derive_input(&input) {
        Ok(val) => val,
        Err(err) => return err.write_errors().into(),
    };
    let mod_path = proc_macro2::TokenStream::from_str(&opts.fracpack_mod).unwrap();

    match &input.data {
        Data::Struct(data) => {
            if let Fields::Named(named) = &data.fields {
                gen_wit_unpack(&mod_path, &input, named)
            } else {
                panic!("WitUnpack only supports named structs");
            }
        }
        Data::Enum(data) => gen_wit_unpack_enum(&mod_path, &input, data),
        _ => panic!("WitUnpack only supports structs and enums"),
    }
}

// ── Code generation ─────────────────────────────────────────────────────

fn gen_wit_pack(
    mod_path: &proc_macro2::TokenStream,
    input: &DeriveInput,
    named: &syn::FieldsNamed,
) -> TokenStream {
    let name = &input.ident;
    let (impl_generics, ty_generics, where_clause) = input.generics.split_for_impl();

    let field_infos: Vec<_> = named.named.iter().map(|f| {
        let ident = f.ident.as_ref().unwrap();
        let ty = &f.ty;
        let cls = classify_type(ty);
        (ident.clone(), ty.clone(), cls)
    }).collect();

    // Build the layout descriptor pairs: (alignment, size) for each field
    let layout_pairs: Vec<_> = field_infos.iter().map(|(_, ty, _)| {
        quote! {
            (<#ty as #mod_path::wit::layout::WitLayout>::wit_alignment(),
             <#ty as #mod_path::wit::layout::WitLayout>::wit_size())
        }
    }).collect();

    // Generate store statements
    let store_stmts: Vec<_> = field_infos.iter().enumerate().map(|(i, (ident, ty, cls))| {
        let field_expr = quote!(self.#ident);
        let idx = syn::Index::from(i);
        let store = store_field_tokens(cls, mod_path, &field_expr, ty);
        quote! {
            {
                let loc = &__locs[#idx];
                #store
            }
        }
    }).collect();

    // Generate accumulate statements
    let accum_stmts: Vec<_> = field_infos.iter().map(|(ident, ty, cls)| {
        let field_expr = quote!(self.#ident);
        accumulate_tokens(cls, mod_path, &field_expr, ty)
    }).collect();

    let expanded = quote! {
        impl #impl_generics #mod_path::wit::layout::WitLayout for #name #ty_generics #where_clause {
            fn wit_alignment() -> u32 {
                let fields: &[(u32, u32)] = &[
                    #(#layout_pairs),*
                ];
                let (_, _, max_align) = #mod_path::wit::layout::compute_struct_layout(fields);
                max_align
            }

            fn wit_size() -> u32 {
                let fields: &[(u32, u32)] = &[
                    #(#layout_pairs),*
                ];
                let (_, total_size, _) = #mod_path::wit::layout::compute_struct_layout(fields);
                total_size
            }
        }

        impl #impl_generics #mod_path::wit::pack::WitPack for #name #ty_generics #where_clause {
            fn wit_accumulate_size(&self, bump: &mut u32) {
                #(#accum_stmts)*
            }

            fn wit_store(&self, packer: &mut #mod_path::wit::pack::WitPacker<'_>, dest: u32) {
                let fields: &[(u32, u32)] = &[
                    #(#layout_pairs),*
                ];
                let (__locs, _, _) = #mod_path::wit::layout::compute_struct_layout(fields);
                #(#store_stmts)*
            }
        }
    };
    expanded.into()
}

fn gen_wit_unpack(
    mod_path: &proc_macro2::TokenStream,
    input: &DeriveInput,
    named: &syn::FieldsNamed,
) -> TokenStream {
    let name = &input.ident;
    let (impl_generics, ty_generics, where_clause) = input.generics.split_for_impl();

    let field_infos: Vec<_> = named.named.iter().map(|f| {
        let ident = f.ident.as_ref().unwrap();
        let ty = &f.ty;
        let cls = classify_type(ty);
        (ident.clone(), ty.clone(), cls)
    }).collect();

    let layout_pairs: Vec<_> = field_infos.iter().map(|(_, ty, _)| {
        quote! {
            (<#ty as #mod_path::wit::layout::WitLayout>::wit_alignment(),
             <#ty as #mod_path::wit::layout::WitLayout>::wit_size())
        }
    }).collect();

    let field_inits: Vec<_> = field_infos.iter().enumerate().map(|(i, (ident, ty, cls))| {
        let idx = syn::Index::from(i);
        let unpack_expr = unpack_field_tokens(cls, mod_path, ty);
        quote! {
            #ident: {
                let sub = view.sub_view(__locs[#idx].offset);
                #unpack_expr
            }
        }
    }).collect();

    let expanded = quote! {
        impl #impl_generics #mod_path::wit::view::WitUnpack for #name #ty_generics #where_clause {
            fn wit_unpack_from(view: &#mod_path::wit::view::WitView<'_>) -> Result<Self, #mod_path::wit::view::WitViewError> {
                let fields: &[(u32, u32)] = &[
                    #(#layout_pairs),*
                ];
                let (__locs, _, _) = #mod_path::wit::layout::compute_struct_layout(fields);
                Ok(#name {
                    #(#field_inits),*
                })
            }
        }
    };
    expanded.into()
}

// ── Enum code generation ───────────────────────────────────────────────

/// Generate store_field_tokens for enum contexts where field bindings are
/// references (due to match ergonomics when matching on `&self`).
/// Wraps scalar field expressions with `*` to dereference.
fn store_field_tokens_enum(
    cls: &WitFieldClass,
    mod_path: &proc_macro2::TokenStream,
    field_expr: &proc_macro2::TokenStream,
    field_ty: &Type,
) -> proc_macro2::TokenStream {
    match cls {
        WitFieldClass::Bool => {
            quote! { packer.store_u8(dest + loc.offset, if *#field_expr { 1 } else { 0 }); }
        }
        WitFieldClass::U8 => quote! { packer.store_u8(dest + loc.offset, *#field_expr); },
        WitFieldClass::I8 => quote! { packer.store_u8(dest + loc.offset, *#field_expr as u8); },
        WitFieldClass::U16 => quote! { packer.store_u16(dest + loc.offset, *#field_expr); },
        WitFieldClass::I16 => quote! { packer.store_u16(dest + loc.offset, *#field_expr as u16); },
        WitFieldClass::U32 => quote! { packer.store_u32(dest + loc.offset, *#field_expr); },
        WitFieldClass::I32 => quote! { packer.store_u32(dest + loc.offset, *#field_expr as u32); },
        WitFieldClass::F32 => quote! { packer.store_f32(dest + loc.offset, *#field_expr); },
        WitFieldClass::U64 => quote! { packer.store_u64(dest + loc.offset, *#field_expr); },
        WitFieldClass::I64 => quote! { packer.store_u64(dest + loc.offset, *#field_expr as u64); },
        WitFieldClass::F64 => quote! { packer.store_f64(dest + loc.offset, *#field_expr); },
        WitFieldClass::Text | WitFieldClass::Vec | WitFieldClass::Struct => {
            // For non-scalar types, the field is already a reference, pass as-is
            quote! {
                <#field_ty as #mod_path::wit::pack::WitPack>::wit_store(#field_expr, packer, dest + loc.offset);
            }
        }
    }
}

/// Generate accumulate_tokens for enum contexts where field bindings are references.
fn accumulate_tokens_enum(
    cls: &WitFieldClass,
    mod_path: &proc_macro2::TokenStream,
    field_expr: &proc_macro2::TokenStream,
    field_ty: &Type,
) -> proc_macro2::TokenStream {
    match cls {
        WitFieldClass::Bool | WitFieldClass::U8 | WitFieldClass::I8 |
        WitFieldClass::U16 | WitFieldClass::I16 |
        WitFieldClass::U32 | WitFieldClass::I32 | WitFieldClass::F32 |
        WitFieldClass::U64 | WitFieldClass::I64 | WitFieldClass::F64 => {
            quote! {} // no-op
        }
        WitFieldClass::Text | WitFieldClass::Vec | WitFieldClass::Struct => {
            // field_expr is already a reference in enum match context
            quote! {
                <#field_ty as #mod_path::wit::pack::WitPack>::wit_accumulate_size(#field_expr, bump);
            }
        }
    }
}

/// Describes a single enum variant for WIT codegen.
struct WitEnumVariant {
    ident: syn::Ident,
    /// The types of the fields in this variant (empty for unit variants).
    field_types: Vec<Type>,
    /// Whether this variant uses named fields (struct variant) vs positional (tuple variant).
    is_named: bool,
    /// Field names for named (struct) variants.
    field_names: Vec<syn::Ident>,
}

fn collect_enum_variants(data: &DataEnum) -> Vec<WitEnumVariant> {
    data.variants
        .iter()
        .map(|var| {
            let ident = var.ident.clone();
            match &var.fields {
                Fields::Unit => WitEnumVariant {
                    ident,
                    field_types: vec![],
                    is_named: false,
                    field_names: vec![],
                },
                Fields::Unnamed(fields) => WitEnumVariant {
                    ident,
                    field_types: fields.unnamed.iter().map(|f| f.ty.clone()).collect(),
                    is_named: false,
                    field_names: vec![],
                },
                Fields::Named(fields) => WitEnumVariant {
                    ident,
                    field_types: fields.named.iter().map(|f| f.ty.clone()).collect(),
                    is_named: true,
                    field_names: fields
                        .named
                        .iter()
                        .map(|f| f.ident.clone().unwrap())
                        .collect(),
                },
            }
        })
        .collect()
}

/// Generate the layout (size, align) expression for a variant's payload.
///
/// - Unit variant: (0, 1)
/// - Single-field tuple variant: use the field's WitLayout directly
/// - Multi-field tuple or struct variant: compute as a record (struct layout)
fn variant_case_layout_tokens(
    mod_path: &proc_macro2::TokenStream,
    var: &WitEnumVariant,
) -> proc_macro2::TokenStream {
    if var.field_types.is_empty() {
        // Unit variant: zero-size payload
        quote! { (0u32, 1u32) }
    } else if var.field_types.len() == 1 {
        // Single-field: use field's layout directly
        let ty = &var.field_types[0];
        quote! {
            (<#ty as #mod_path::wit::layout::WitLayout>::wit_size(),
             <#ty as #mod_path::wit::layout::WitLayout>::wit_alignment())
        }
    } else {
        // Multi-field: compute as record
        let field_pairs: Vec<_> = var.field_types.iter().map(|ty| {
            quote! {
                (<#ty as #mod_path::wit::layout::WitLayout>::wit_alignment(),
                 <#ty as #mod_path::wit::layout::WitLayout>::wit_size())
            }
        }).collect();
        quote! {
            {
                let fields: &[(u32, u32)] = &[ #(#field_pairs),* ];
                let (_, total_size, max_align) = #mod_path::wit::layout::compute_struct_layout(fields);
                (total_size, max_align)
            }
        }
    }
}

fn gen_wit_pack_enum(
    mod_path: &proc_macro2::TokenStream,
    input: &DeriveInput,
    data: &DataEnum,
) -> TokenStream {
    let name = &input.ident;
    let (impl_generics, ty_generics, where_clause) = input.generics.split_for_impl();
    let variants = collect_enum_variants(data);
    let case_count = variants.len();

    // Build case layout pairs: (size, align) for each variant's payload
    let case_layout_exprs: Vec<_> = variants.iter().map(|v| {
        variant_case_layout_tokens(mod_path, v)
    }).collect();

    // Generate match arms for wit_store
    let store_arms: Vec<_> = variants.iter().enumerate().map(|(i, var)| {
        let var_ident = &var.ident;
        let disc_idx = i as u32;

        if var.field_types.is_empty() {
            // Unit variant: just write discriminant
            if var.is_named {
                quote! { #name::#var_ident {} => { __disc = #disc_idx; } }
            } else {
                quote! { #name::#var_ident => { __disc = #disc_idx; } }
            }
        } else if var.field_types.len() == 1 && !var.is_named {
            // Single-field tuple variant
            let field_ty = &var.field_types[0];
            let cls = classify_type(field_ty);
            let field_expr = quote!(field_0);
            let store = store_field_tokens_enum(&cls, mod_path, &field_expr, field_ty);
            quote! {
                #name::#var_ident(field_0) => {
                    __disc = #disc_idx;
                    // Store payload at payload_offset
                    let loc = #mod_path::wit::layout::WitFieldLoc {
                        offset: __payload_offset,
                        size: <#field_ty as #mod_path::wit::layout::WitLayout>::wit_size(),
                        alignment: <#field_ty as #mod_path::wit::layout::WitLayout>::wit_alignment(),
                    };
                    #store
                }
            }
        } else if var.is_named {
            // Struct variant: lay out fields as a record
            let field_names = &var.field_names;
            let field_tys = &var.field_types;
            let field_layout_pairs: Vec<_> = field_tys.iter().map(|ty| {
                quote! {
                    (<#ty as #mod_path::wit::layout::WitLayout>::wit_alignment(),
                     <#ty as #mod_path::wit::layout::WitLayout>::wit_size())
                }
            }).collect();
            let store_stmts: Vec<_> = field_names.iter().enumerate().map(|(fi, fname)| {
                let fty = &field_tys[fi];
                let cls = classify_type(fty);
                let field_expr = quote!(#fname);
                let store = store_field_tokens_enum(&cls, mod_path, &field_expr, fty);
                let fidx = syn::Index::from(fi);
                quote! {
                    {
                        let loc = #mod_path::wit::layout::WitFieldLoc {
                            offset: __payload_offset + __field_locs[#fidx].offset,
                            size: __field_locs[#fidx].size,
                            alignment: __field_locs[#fidx].alignment,
                        };
                        #store
                    }
                }
            }).collect();
            quote! {
                #name::#var_ident { #(#field_names),* } => {
                    __disc = #disc_idx;
                    let __field_desc: &[(u32, u32)] = &[ #(#field_layout_pairs),* ];
                    let (__field_locs, _, _) = #mod_path::wit::layout::compute_struct_layout(__field_desc);
                    #(#store_stmts)*
                }
            }
        } else {
            // Multi-field tuple variant: lay out fields as a record
            let field_tys = &var.field_types;
            let binding_names: Vec<_> = (0..field_tys.len()).map(|i| {
                syn::Ident::new(&format!("field_{}", i), proc_macro2::Span::call_site())
            }).collect();
            let field_layout_pairs: Vec<_> = field_tys.iter().map(|ty| {
                quote! {
                    (<#ty as #mod_path::wit::layout::WitLayout>::wit_alignment(),
                     <#ty as #mod_path::wit::layout::WitLayout>::wit_size())
                }
            }).collect();
            let store_stmts: Vec<_> = binding_names.iter().enumerate().map(|(fi, bname)| {
                let fty = &field_tys[fi];
                let cls = classify_type(fty);
                let field_expr = quote!(#bname);
                let store = store_field_tokens_enum(&cls, mod_path, &field_expr, fty);
                let fidx = syn::Index::from(fi);
                quote! {
                    {
                        let loc = #mod_path::wit::layout::WitFieldLoc {
                            offset: __payload_offset + __field_locs[#fidx].offset,
                            size: __field_locs[#fidx].size,
                            alignment: __field_locs[#fidx].alignment,
                        };
                        #store
                    }
                }
            }).collect();
            quote! {
                #name::#var_ident(#(#binding_names),*) => {
                    __disc = #disc_idx;
                    let __field_desc: &[(u32, u32)] = &[ #(#field_layout_pairs),* ];
                    let (__field_locs, _, _) = #mod_path::wit::layout::compute_struct_layout(__field_desc);
                    #(#store_stmts)*
                }
            }
        }
    }).collect();

    // Generate match arms for wit_accumulate_size
    let accum_arms: Vec<_> = variants.iter().enumerate().map(|(i, var)| {
        let var_ident = &var.ident;
        let _ = i;

        if var.field_types.is_empty() {
            if var.is_named {
                quote! { #name::#var_ident {} => {} }
            } else {
                quote! { #name::#var_ident => {} }
            }
        } else if var.field_types.len() == 1 && !var.is_named {
            let field_ty = &var.field_types[0];
            let cls = classify_type(field_ty);
            let field_expr = quote!(field_0);
            let accum = accumulate_tokens_enum(&cls, mod_path, &field_expr, field_ty);
            quote! { #name::#var_ident(field_0) => { #accum } }
        } else if var.is_named {
            let field_names = &var.field_names;
            let accum_stmts: Vec<_> = var.field_names.iter().zip(var.field_types.iter()).map(|(fname, fty)| {
                let cls = classify_type(fty);
                let field_expr = quote!(#fname);
                accumulate_tokens_enum(&cls, mod_path, &field_expr, fty)
            }).collect();
            quote! { #name::#var_ident { #(#field_names),* } => { #(#accum_stmts)* } }
        } else {
            let binding_names: Vec<_> = (0..var.field_types.len()).map(|i| {
                syn::Ident::new(&format!("field_{}", i), proc_macro2::Span::call_site())
            }).collect();
            let accum_stmts: Vec<_> = binding_names.iter().zip(var.field_types.iter()).map(|(bname, fty)| {
                let cls = classify_type(fty);
                let field_expr = quote!(#bname);
                accumulate_tokens_enum(&cls, mod_path, &field_expr, fty)
            }).collect();
            quote! { #name::#var_ident(#(#binding_names),*) => { #(#accum_stmts)* } }
        }
    }).collect();

    // Discriminant store expression depends on disc size
    let disc_store = if case_count <= 256 {
        quote! { packer.store_u8(dest, __disc as u8); }
    } else if case_count <= 65536 {
        quote! { packer.store_u16(dest, __disc as u16); }
    } else {
        quote! { packer.store_u32(dest, __disc); }
    };

    let expanded = quote! {
        impl #impl_generics #mod_path::wit::layout::WitLayout for #name #ty_generics #where_clause {
            fn wit_alignment() -> u32 {
                let cases: &[(u32, u32)] = &[ #(#case_layout_exprs),* ];
                let (_, align) = #mod_path::wit::layout::variant_layout(#case_count, cases);
                align
            }

            fn wit_size() -> u32 {
                let cases: &[(u32, u32)] = &[ #(#case_layout_exprs),* ];
                let (size, _) = #mod_path::wit::layout::variant_layout(#case_count, cases);
                size
            }
        }

        impl #impl_generics #mod_path::wit::pack::WitPack for #name #ty_generics #where_clause {
            fn wit_accumulate_size(&self, bump: &mut u32) {
                match self {
                    #(#accum_arms)*
                }
            }

            fn wit_store(&self, packer: &mut #mod_path::wit::pack::WitPacker<'_>, dest: u32) {
                // Compute payload offset
                let cases: &[(u32, u32)] = &[ #(#case_layout_exprs),* ];
                let mut __max_payload_align: u32 = 1;
                for &(_, a) in cases {
                    if a > __max_payload_align { __max_payload_align = a; }
                }
                let __payload_offset = #mod_path::wit::layout::variant_payload_offset(#case_count, __max_payload_align);

                let mut __disc: u32 = 0;
                match self {
                    #(#store_arms)*
                }
                #disc_store
            }
        }
    };
    expanded.into()
}

fn gen_wit_unpack_enum(
    mod_path: &proc_macro2::TokenStream,
    input: &DeriveInput,
    data: &DataEnum,
) -> TokenStream {
    let name = &input.ident;
    let (impl_generics, ty_generics, where_clause) = input.generics.split_for_impl();
    let variants = collect_enum_variants(data);
    let case_count = variants.len();

    let case_layout_exprs: Vec<_> = variants.iter().map(|v| {
        variant_case_layout_tokens(mod_path, v)
    }).collect();

    // Generate match arms for unpacking
    let unpack_arms: Vec<_> = variants.iter().enumerate().map(|(i, var)| {
        let var_ident = &var.ident;
        let disc_idx = i as u32;

        if var.field_types.is_empty() {
            // Unit variant
            if var.is_named {
                quote! { #disc_idx => Ok(#name::#var_ident {}), }
            } else {
                quote! { #disc_idx => Ok(#name::#var_ident), }
            }
        } else if var.field_types.len() == 1 && !var.is_named {
            // Single-field tuple variant
            let field_ty = &var.field_types[0];
            let cls = classify_type(field_ty);
            let unpack_expr = unpack_field_tokens(&cls, mod_path, field_ty);
            quote! {
                #disc_idx => {
                    let sub = __payload_view;
                    Ok(#name::#var_ident(#unpack_expr))
                },
            }
        } else if var.is_named {
            // Struct variant
            let field_names = &var.field_names;
            let field_tys = &var.field_types;
            let field_layout_pairs: Vec<_> = field_tys.iter().map(|ty| {
                quote! {
                    (<#ty as #mod_path::wit::layout::WitLayout>::wit_alignment(),
                     <#ty as #mod_path::wit::layout::WitLayout>::wit_size())
                }
            }).collect();
            let field_inits: Vec<_> = field_names.iter().enumerate().map(|(fi, fname)| {
                let fty = &field_tys[fi];
                let cls = classify_type(fty);
                let unpack_expr = unpack_field_tokens(&cls, mod_path, fty);
                let fidx = syn::Index::from(fi);
                quote! {
                    #fname: {
                        let sub = __payload_view.sub_view(__field_locs[#fidx].offset);
                        #unpack_expr
                    }
                }
            }).collect();
            quote! {
                #disc_idx => {
                    let __field_desc: &[(u32, u32)] = &[ #(#field_layout_pairs),* ];
                    let (__field_locs, _, _) = #mod_path::wit::layout::compute_struct_layout(__field_desc);
                    Ok(#name::#var_ident {
                        #(#field_inits),*
                    })
                },
            }
        } else {
            // Multi-field tuple variant
            let field_tys = &var.field_types;
            let field_layout_pairs: Vec<_> = field_tys.iter().map(|ty| {
                quote! {
                    (<#ty as #mod_path::wit::layout::WitLayout>::wit_alignment(),
                     <#ty as #mod_path::wit::layout::WitLayout>::wit_size())
                }
            }).collect();
            let field_exprs: Vec<_> = field_tys.iter().enumerate().map(|(fi, fty)| {
                let cls = classify_type(fty);
                let unpack_expr = unpack_field_tokens(&cls, mod_path, fty);
                let fidx = syn::Index::from(fi);
                quote! {
                    {
                        let sub = __payload_view.sub_view(__field_locs[#fidx].offset);
                        #unpack_expr
                    }
                }
            }).collect();
            quote! {
                #disc_idx => {
                    let __field_desc: &[(u32, u32)] = &[ #(#field_layout_pairs),* ];
                    let (__field_locs, _, _) = #mod_path::wit::layout::compute_struct_layout(__field_desc);
                    Ok(#name::#var_ident(
                        #(#field_exprs),*
                    ))
                },
            }
        }
    }).collect();

    let max_disc = (case_count - 1) as u32;

    let expanded = quote! {
        impl #impl_generics #mod_path::wit::view::WitUnpack for #name #ty_generics #where_clause {
            fn wit_unpack_from(view: &#mod_path::wit::view::WitView<'_>) -> Result<Self, #mod_path::wit::view::WitViewError> {
                let cases: &[(u32, u32)] = &[ #(#case_layout_exprs),* ];
                let __disc = view.read_variant_discriminant(0, #case_count);
                let mut __max_payload_align: u32 = 1;
                for &(_, a) in cases {
                    if a > __max_payload_align { __max_payload_align = a; }
                }
                let __payload_offset = #mod_path::wit::layout::variant_payload_offset(#case_count, __max_payload_align);
                let __payload_view = view.sub_view(__payload_offset);

                match __disc {
                    #(#unpack_arms)*
                    other => Err(#mod_path::wit::view::WitViewError::InvalidDiscriminant {
                        value: other,
                        max: #max_disc,
                    }),
                }
            }
        }
    };
    expanded.into()
}

// ── WitView derive macro ───────────────────────────────────────────────

pub fn wit_view_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    let opts = match Options::from_derive_input(&input) {
        Ok(val) => val,
        Err(err) => return err.write_errors().into(),
    };
    let mod_path = proc_macro2::TokenStream::from_str(&opts.fracpack_mod).unwrap();

    match &input.data {
        Data::Struct(data) => {
            if let Fields::Named(named) = &data.fields {
                gen_wit_view(&mod_path, &input, named)
            } else {
                panic!("WitView only supports named structs");
            }
        }
        _ => panic!("WitView only supports structs"),
    }
}

/// Return type for a WitView accessor.
fn wit_view_return_type(
    cls: &WitFieldClass,
    mod_path: &proc_macro2::TokenStream,
) -> proc_macro2::TokenStream {
    match cls {
        WitFieldClass::Bool => quote! { bool },
        WitFieldClass::U8 => quote! { u8 },
        WitFieldClass::I8 => quote! { i8 },
        WitFieldClass::U16 => quote! { u16 },
        WitFieldClass::I16 => quote! { i16 },
        WitFieldClass::U32 => quote! { u32 },
        WitFieldClass::I32 => quote! { i32 },
        WitFieldClass::F32 => quote! { f32 },
        WitFieldClass::U64 => quote! { u64 },
        WitFieldClass::I64 => quote! { i64 },
        WitFieldClass::F64 => quote! { f64 },
        WitFieldClass::Text => quote! { Result<&'a str, #mod_path::wit::view::WitViewError> },
        WitFieldClass::Vec | WitFieldClass::Struct => {
            quote! { #mod_path::wit::view::WitView<'a> }
        }
    }
}

/// Generate the view accessor expression for a WitView field.
fn wit_view_field_tokens(
    cls: &WitFieldClass,
) -> proc_macro2::TokenStream {
    match cls {
        WitFieldClass::Bool => quote! { sub.read_bool(0) },
        WitFieldClass::U8 => quote! { sub.read_u8(0) },
        WitFieldClass::I8 => quote! { sub.read_i8(0) },
        WitFieldClass::U16 => quote! { sub.read_u16(0) },
        WitFieldClass::I16 => quote! { sub.read_i16(0) },
        WitFieldClass::U32 => quote! { sub.read_u32(0) },
        WitFieldClass::I32 => quote! { sub.read_i32(0) },
        WitFieldClass::F32 => quote! { sub.read_f32(0) },
        WitFieldClass::U64 => quote! { sub.read_u64(0) },
        WitFieldClass::I64 => quote! { sub.read_i64(0) },
        WitFieldClass::F64 => quote! { sub.read_f64(0) },
        WitFieldClass::Text => quote! { sub.read_string(0) },
        WitFieldClass::Vec | WitFieldClass::Struct => {
            quote! { sub }
        }
    }
}

fn gen_wit_view(
    mod_path: &proc_macro2::TokenStream,
    input: &DeriveInput,
    named: &syn::FieldsNamed,
) -> TokenStream {
    let name = &input.ident;
    let vis = &input.vis;
    let view_name = syn::Ident::new(&format!("{}WitView", name), name.span());

    let field_infos: Vec<_> = named.named.iter().map(|f| {
        let ident = f.ident.as_ref().unwrap();
        let ty = &f.ty;
        let cls = classify_type(ty);
        (ident.clone(), ty.clone(), cls)
    }).collect();

    let layout_pairs: Vec<_> = field_infos.iter().map(|(_, ty, _)| {
        quote! {
            (<#ty as #mod_path::wit::layout::WitLayout>::wit_alignment(),
             <#ty as #mod_path::wit::layout::WitLayout>::wit_size())
        }
    }).collect();

    let field_accessors: Vec<_> = field_infos.iter().enumerate().map(|(i, (ident, _, cls))| {
        let ret_ty = wit_view_return_type(cls, mod_path);
        let expr = wit_view_field_tokens(cls);
        let idx = syn::Index::from(i);
        quote! {
            pub fn #ident(&self) -> #ret_ty {
                let fields: &[(u32, u32)] = &[
                    #(#layout_pairs),*
                ];
                let (__locs, _, _) = #mod_path::wit::layout::compute_struct_layout(fields);
                let sub = self.view.sub_view(__locs[#idx].offset);
                #expr
            }
        }
    }).collect();

    let expanded = quote! {
        #[derive(Clone, Copy)]
        #vis struct #view_name<'a> {
            view: #mod_path::wit::view::WitView<'a>,
        }

        impl<'a> #view_name<'a> {
            /// Create a view from a WIT-encoded buffer.
            pub fn from_buffer(data: &'a [u8]) -> Self {
                let view = #mod_path::wit::view::WitView::from_buffer(data);
                #view_name { view }
            }

            /// Create a view from an existing WitView.
            pub fn from_view(view: #mod_path::wit::view::WitView<'a>) -> Self {
                #view_name { view }
            }

            #(#field_accessors)*
        }
    };
    expanded.into()
}
