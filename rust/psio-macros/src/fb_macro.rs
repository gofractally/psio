//! Derive macros for FlatBuffers serialization (FbPack, FbUnpack).
//!
//! Generates `impl FbPack` and `impl FbUnpack` for named structs,
//! computing vtable slots from field order and emitting the appropriate
//! builder / view code.

use darling::FromDeriveInput;
use proc_macro::TokenStream;
use quote::quote;
use std::str::FromStr;
use syn::{parse_macro_input, Data, DeriveInput, Fields, Type};

use crate::fracpack_macro::Options;

// ── Type classification ─────────────────────────────────────────────────

enum FbFieldClass {
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
    VecScalar,   // Vec<scalar> or Vec<bool>
    VecString,
    VecStruct,
    Struct,
}

fn classify_type(ty: &Type) -> FbFieldClass {
    let ty_str = quote!(#ty).to_string().replace(' ', "");
    match ty_str.as_str() {
        "bool" => FbFieldClass::Bool,
        "u8" => FbFieldClass::U8,
        "i8" => FbFieldClass::I8,
        "u16" => FbFieldClass::U16,
        "i16" => FbFieldClass::I16,
        "u32" => FbFieldClass::U32,
        "i32" => FbFieldClass::I32,
        "f32" => FbFieldClass::F32,
        "u64" => FbFieldClass::U64,
        "i64" => FbFieldClass::I64,
        "f64" => FbFieldClass::F64,
        "String" => FbFieldClass::Text,
        _ => {
            if let Type::Path(tp) = ty {
                let seg = tp.path.segments.last().unwrap();
                if seg.ident == "Vec" {
                    if let syn::PathArguments::AngleBracketed(args) = &seg.arguments {
                        if let Some(syn::GenericArgument::Type(inner)) = args.args.first() {
                            let inner_str = quote!(#inner).to_string().replace(' ', "");
                            return match inner_str.as_str() {
                                "bool" | "u8" | "i8" | "u16" | "i16" | "u32" | "i32" | "f32" |
                                "u64" | "i64" | "f64" => FbFieldClass::VecScalar,
                                "String" => FbFieldClass::VecString,
                                _ => FbFieldClass::VecStruct,
                            };
                        }
                    }
                }
            }
            FbFieldClass::Struct
        }
    }
}

// ── Pack code generation ────────────────────────────────────────────────

/// Generate the pre-creation statement for offset types (strings, vecs, nested tables).
/// Returns (pre_statement, offset_var_name) or None for scalars.
fn pre_create_tokens(
    cls: &FbFieldClass,
    mod_path: &proc_macro2::TokenStream,
    field_expr: &proc_macro2::TokenStream,
    idx: usize,
) -> Option<(proc_macro2::TokenStream, proc_macro2::Ident)> {
    let off_var = syn::Ident::new(&format!("__off_{}", idx), proc_macro2::Span::call_site());

    let stmt = match cls {
        FbFieldClass::Text => {
            quote! {
                let #off_var = if #field_expr.is_empty() { 0u32 } else { b.create_string(&#field_expr) };
            }
        }
        FbFieldClass::VecScalar => {
            quote! {
                let #off_var = if #field_expr.is_empty() {
                    0u32
                } else {
                    <_ as #mod_path::flatbuf::FbPackElement>::fb_write_vec(&#field_expr[..], b)
                };
            }
        }
        FbFieldClass::VecString => {
            quote! {
                let #off_var = if #field_expr.is_empty() {
                    0u32
                } else {
                    <String as #mod_path::flatbuf::FbPackElement>::fb_write_vec(&#field_expr[..], b)
                };
            }
        }
        FbFieldClass::VecStruct => {
            // For Vec<T> where T: FbPack, we write each element as a table and
            // collect offsets, then create a vector of offsets.
            quote! {
                let #off_var = if #field_expr.is_empty() {
                    0u32
                } else {
                    let mut __elem_offs: Vec<u32> = Vec::with_capacity(#field_expr.len());
                    for __elem in #field_expr.iter() {
                        __elem_offs.push(#mod_path::flatbuf::FbPack::fb_write(__elem, b));
                    }
                    b.create_vec_offsets(&__elem_offs)
                };
            }
        }
        FbFieldClass::Struct => {
            quote! {
                let #off_var = #mod_path::flatbuf::FbPack::fb_write(&#field_expr, b);
            }
        }
        _ => return None, // scalar
    };

    Some((stmt, off_var))
}

/// Generate the add-field statement inside start_table/end_table.
fn add_field_tokens(
    cls: &FbFieldClass,
    field_expr: &proc_macro2::TokenStream,
    slot: u16,
    off_var: Option<&syn::Ident>,
) -> proc_macro2::TokenStream {
    let vt = 4 + 2 * slot;
    let vt_lit = syn::LitInt::new(&vt.to_string(), proc_macro2::Span::call_site());

    match cls {
        FbFieldClass::Bool => quote! { b.add_scalar_bool(#vt_lit, #field_expr, false); },
        FbFieldClass::U8 => quote! { b.add_scalar_u8(#vt_lit, #field_expr, 0); },
        FbFieldClass::I8 => quote! { b.add_scalar_i8(#vt_lit, #field_expr, 0); },
        FbFieldClass::U16 => quote! { b.add_scalar_u16(#vt_lit, #field_expr, 0); },
        FbFieldClass::I16 => quote! { b.add_scalar_i16(#vt_lit, #field_expr, 0); },
        FbFieldClass::U32 => quote! { b.add_scalar_u32(#vt_lit, #field_expr, 0); },
        FbFieldClass::I32 => quote! { b.add_scalar_i32(#vt_lit, #field_expr, 0); },
        FbFieldClass::F32 => quote! { b.add_scalar_f32(#vt_lit, #field_expr, 0.0); },
        FbFieldClass::U64 => quote! { b.add_scalar_u64(#vt_lit, #field_expr, 0); },
        FbFieldClass::I64 => quote! { b.add_scalar_i64(#vt_lit, #field_expr, 0); },
        FbFieldClass::F64 => quote! { b.add_scalar_f64(#vt_lit, #field_expr, 0.0); },
        _ => {
            let ov = off_var.unwrap();
            quote! { b.add_offset_field(#vt_lit, #ov); }
        }
    }
}

// ── Unpack code generation ──────────────────────────────────────────────

fn unpack_field_tokens(
    cls: &FbFieldClass,
    mod_path: &proc_macro2::TokenStream,
    slot: u16,
    field_ty: &Type,
) -> proc_macro2::TokenStream {
    match cls {
        FbFieldClass::Bool => quote! { view.read_bool(#slot, false) },
        FbFieldClass::U8 => quote! { view.read_u8(#slot, 0) },
        FbFieldClass::I8 => quote! { view.read_i8(#slot, 0) },
        FbFieldClass::U16 => quote! { view.read_u16(#slot, 0) },
        FbFieldClass::I16 => quote! { view.read_i16(#slot, 0) },
        FbFieldClass::U32 => quote! { view.read_u32(#slot, 0) },
        FbFieldClass::I32 => quote! { view.read_i32(#slot, 0) },
        FbFieldClass::F32 => quote! { view.read_f32(#slot, 0.0) },
        FbFieldClass::U64 => quote! { view.read_u64(#slot, 0) },
        FbFieldClass::I64 => quote! { view.read_i64(#slot, 0) },
        FbFieldClass::F64 => quote! { view.read_f64(#slot, 0.0) },
        FbFieldClass::Text => {
            quote! { view.read_str_or_empty(#slot).to_string() }
        }
        FbFieldClass::VecScalar | FbFieldClass::VecString => {
            // Extract inner type from Vec<T>
            let inner_ty = extract_vec_inner(field_ty);
            quote! {
                match view.read_vec(#slot) {
                    Some(vec_view) => <#inner_ty as #mod_path::flatbuf::FbUnpackElement>::fb_unpack_vec(&vec_view)
                        .unwrap_or_default(),
                    None => Vec::new(),
                }
            }
        }
        FbFieldClass::VecStruct => {
            let inner_ty = extract_vec_inner(field_ty);
            quote! {
                match view.read_vec(#slot) {
                    Some(vec_view) => {
                        let mut __result = Vec::with_capacity(vec_view.len() as usize);
                        for __i in 0..vec_view.len() {
                            if let Some(__tbl) = vec_view.get_table(__i) {
                                __result.push(<#inner_ty as #mod_path::flatbuf::FbUnpack>::fb_unpack_view(&__tbl)
                                    .unwrap_or_else(|_| panic!("failed to unpack nested struct")));
                            }
                        }
                        __result
                    }
                    None => Vec::new(),
                }
            }
        }
        FbFieldClass::Struct => {
            quote! {
                match view.read_table(#slot) {
                    Some(sub_view) => <#field_ty as #mod_path::flatbuf::FbUnpack>::fb_unpack_view(&sub_view)
                        .unwrap_or_else(|_| panic!("failed to unpack nested struct")),
                    None => <#field_ty as #mod_path::flatbuf::FbUnpack>::fb_unpack_view(&view)
                        .unwrap_or_else(|_| panic!("failed to unpack nested struct")),
                }
            }
        }
    }
}

fn extract_vec_inner(ty: &Type) -> proc_macro2::TokenStream {
    if let Type::Path(tp) = ty {
        let seg = tp.path.segments.last().unwrap();
        if let syn::PathArguments::AngleBracketed(args) = &seg.arguments {
            if let Some(syn::GenericArgument::Type(inner)) = args.args.first() {
                return quote!(#inner);
            }
        }
    }
    quote!(#ty)
}

// ── Entry points ────────────────────────────────────────────────────────

pub fn fb_pack_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    let opts = match Options::from_derive_input(&input) {
        Ok(val) => val,
        Err(err) => return err.write_errors().into(),
    };
    let mod_path = proc_macro2::TokenStream::from_str(&opts.fracpack_mod).unwrap();

    match &input.data {
        Data::Struct(data) => {
            if let Fields::Named(named) = &data.fields {
                gen_fb_pack(&mod_path, &input, named)
            } else {
                panic!("FbPack only supports named structs");
            }
        }
        _ => panic!("FbPack only supports structs"),
    }
}

pub fn fb_unpack_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    let opts = match Options::from_derive_input(&input) {
        Ok(val) => val,
        Err(err) => return err.write_errors().into(),
    };
    let mod_path = proc_macro2::TokenStream::from_str(&opts.fracpack_mod).unwrap();

    match &input.data {
        Data::Struct(data) => {
            if let Fields::Named(named) = &data.fields {
                gen_fb_unpack(&mod_path, &input, named)
            } else {
                panic!("FbUnpack only supports named structs");
            }
        }
        _ => panic!("FbUnpack only supports structs"),
    }
}

// ── Code generation ─────────────────────────────────────────────────────

fn gen_fb_pack(
    mod_path: &proc_macro2::TokenStream,
    input: &DeriveInput,
    named: &syn::FieldsNamed,
) -> TokenStream {
    let name = &input.ident;
    let (impl_generics, ty_generics, where_clause) = input.generics.split_for_impl();

    let field_infos: Vec<_> = named.named.iter().enumerate().map(|(i, f)| {
        let ident = f.ident.as_ref().unwrap();
        let ty = &f.ty;
        let cls = classify_type(ty);
        (i, ident, ty, cls)
    }).collect();

    // Pre-create offset sub-objects (strings, vecs, nested tables)
    let mut pre_creates = Vec::new();
    let mut off_vars: Vec<Option<syn::Ident>> = Vec::new();

    for (i, ident, _, cls) in &field_infos {
        let field_expr = quote!(self.#ident);
        if let Some((stmt, var)) = pre_create_tokens(cls, mod_path, &field_expr, *i) {
            pre_creates.push(stmt);
            off_vars.push(Some(var));
        } else {
            off_vars.push(None);
        }
    }

    // Add fields to table (smallest alignment first for back-to-front packing)
    let add_stmts: Vec<_> = field_infos.iter().map(|(i, ident, _, cls)| {
        let field_expr = quote!(self.#ident);
        let slot = *i as u16;
        add_field_tokens(cls, &field_expr, slot, off_vars[*i].as_ref())
    }).collect();

    let expanded = quote! {
        impl #impl_generics #mod_path::flatbuf::FbPack for #name #ty_generics #where_clause {
            fn fb_write(&self, b: &mut #mod_path::flatbuf::FbBuilder) -> u32 {
                // Phase 1: Pre-create sub-objects
                #(#pre_creates)*

                // Phase 2: Build table
                b.start_table();
                #(#add_stmts)*
                b.end_table()
            }
        }
    };
    expanded.into()
}

fn gen_fb_unpack(
    mod_path: &proc_macro2::TokenStream,
    input: &DeriveInput,
    named: &syn::FieldsNamed,
) -> TokenStream {
    let name = &input.ident;
    let (impl_generics, ty_generics, where_clause) = input.generics.split_for_impl();

    let field_infos: Vec<_> = named.named.iter().enumerate().map(|(i, f)| {
        let ident = f.ident.as_ref().unwrap();
        let ty = &f.ty;
        let cls = classify_type(ty);
        (i, ident, ty, cls)
    }).collect();

    let field_inits: Vec<_> = field_infos.iter().map(|(i, ident, ty, cls)| {
        let slot = *i as u16;
        let expr = unpack_field_tokens(cls, mod_path, slot, ty);
        quote! { #ident: #expr }
    }).collect();

    let expanded = quote! {
        impl #impl_generics #mod_path::flatbuf::FbUnpack for #name #ty_generics #where_clause {
            fn fb_unpack(data: &[u8]) -> Result<Self, #mod_path::flatbuf::FbUnpackError> {
                let view = #mod_path::flatbuf::view::FbView::from_buffer(data)
                    .map_err(|e| #mod_path::flatbuf::FbUnpackError::InvalidBuffer(format!("{}", e)))?;
                Self::fb_unpack_view(&view)
            }

            fn fb_unpack_view(view: &#mod_path::flatbuf::view::FbView<'_>) -> Result<Self, #mod_path::flatbuf::FbUnpackError> {
                Ok(#name {
                    #(#field_inits),*
                })
            }
        }
    };
    expanded.into()
}

// ── FbView derive macro ────────────────────────────────────────────────

pub fn fb_view_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    let opts = match Options::from_derive_input(&input) {
        Ok(val) => val,
        Err(err) => return err.write_errors().into(),
    };
    let mod_path = proc_macro2::TokenStream::from_str(&opts.fracpack_mod).unwrap();

    match &input.data {
        Data::Struct(data) => {
            if let Fields::Named(named) = &data.fields {
                gen_fb_view(&mod_path, &input, named)
            } else {
                panic!("FbView only supports named structs");
            }
        }
        _ => panic!("FbView only supports structs"),
    }
}

/// Return type for a FbView accessor.
fn fb_view_return_type(
    cls: &FbFieldClass,
    mod_path: &proc_macro2::TokenStream,
) -> proc_macro2::TokenStream {
    match cls {
        FbFieldClass::Bool => quote! { bool },
        FbFieldClass::U8 => quote! { u8 },
        FbFieldClass::I8 => quote! { i8 },
        FbFieldClass::U16 => quote! { u16 },
        FbFieldClass::I16 => quote! { i16 },
        FbFieldClass::U32 => quote! { u32 },
        FbFieldClass::I32 => quote! { i32 },
        FbFieldClass::F32 => quote! { f32 },
        FbFieldClass::U64 => quote! { u64 },
        FbFieldClass::I64 => quote! { i64 },
        FbFieldClass::F64 => quote! { f64 },
        FbFieldClass::Text => quote! { &'a str },
        FbFieldClass::Struct => quote! { Option<#mod_path::flatbuf::view::FbView<'a>> },
        FbFieldClass::VecScalar | FbFieldClass::VecString | FbFieldClass::VecStruct => {
            quote! { Option<#mod_path::flatbuf::view::FbVecView<'a>> }
        }
    }
}

/// Generate the view accessor expression for a FbView field.
fn fb_view_field_tokens(
    cls: &FbFieldClass,
    slot: u16,
) -> proc_macro2::TokenStream {
    match cls {
        FbFieldClass::Bool => quote! { self.view.read_bool(#slot, false) },
        FbFieldClass::U8 => quote! { self.view.read_u8(#slot, 0) },
        FbFieldClass::I8 => quote! { self.view.read_i8(#slot, 0) },
        FbFieldClass::U16 => quote! { self.view.read_u16(#slot, 0) },
        FbFieldClass::I16 => quote! { self.view.read_i16(#slot, 0) },
        FbFieldClass::U32 => quote! { self.view.read_u32(#slot, 0) },
        FbFieldClass::I32 => quote! { self.view.read_i32(#slot, 0) },
        FbFieldClass::F32 => quote! { self.view.read_f32(#slot, 0.0) },
        FbFieldClass::U64 => quote! { self.view.read_u64(#slot, 0) },
        FbFieldClass::I64 => quote! { self.view.read_i64(#slot, 0) },
        FbFieldClass::F64 => quote! { self.view.read_f64(#slot, 0.0) },
        FbFieldClass::Text => quote! { self.view.read_str_or_empty(#slot) },
        FbFieldClass::Struct => quote! { self.view.read_table(#slot) },
        FbFieldClass::VecScalar | FbFieldClass::VecString | FbFieldClass::VecStruct => {
            quote! { self.view.read_vec(#slot) }
        }
    }
}

fn gen_fb_view(
    mod_path: &proc_macro2::TokenStream,
    input: &DeriveInput,
    named: &syn::FieldsNamed,
) -> TokenStream {
    let name = &input.ident;
    let vis = &input.vis;
    let view_name = syn::Ident::new(&format!("{}FbView", name), name.span());

    let field_infos: Vec<_> = named.named.iter().enumerate().map(|(i, f)| {
        let ident = f.ident.as_ref().unwrap();
        let ty = &f.ty;
        let cls = classify_type(ty);
        (i, ident, ty, cls)
    }).collect();

    let field_accessors: Vec<_> = field_infos.iter().map(|(i, ident, _, cls)| {
        let slot = *i as u16;
        let ret_ty = fb_view_return_type(cls, mod_path);
        let expr = fb_view_field_tokens(cls, slot);
        quote! {
            pub fn #ident(&self) -> #ret_ty {
                #expr
            }
        }
    }).collect();

    let expanded = quote! {
        #[derive(Debug, Clone, Copy)]
        #vis struct #view_name<'a> {
            view: #mod_path::flatbuf::view::FbView<'a>,
        }

        impl<'a> #view_name<'a> {
            /// Create a view from a complete FlatBuffer.
            pub fn from_buffer(data: &'a [u8]) -> Result<Self, #mod_path::flatbuf::view::FbViewError> {
                let view = #mod_path::flatbuf::view::FbView::from_buffer(data)?;
                Ok(#view_name { view })
            }

            /// Create a view from an existing FbView.
            pub fn from_view(view: #mod_path::flatbuf::view::FbView<'a>) -> Self {
                #view_name { view }
            }

            #(#field_accessors)*
        }
    };
    expanded.into()
}
