//! Derive macros for Cap'n Proto serialization (CapnpPack, CapnpUnpack).
//!
//! Generates `impl CapnpPack` and `impl CapnpUnpack` for named structs,
//! computing the Cap'n Proto layout from field types and emitting the
//! appropriate pack/unpack code.

use darling::FromDeriveInput;
use proc_macro::TokenStream;
use quote::quote;
use std::str::FromStr;
use syn::{parse_macro_input, Data, DeriveInput, Fields, Type};

use crate::fracpack_macro::Options;

// ── Type classification ─────────────────────────────────────────────────

/// Classify a Rust type into its Cap'n Proto field kind for code generation.
enum CapnpFieldClass {
    Bool,
    ScalarU8,
    ScalarI8,
    ScalarU16,
    ScalarI16,
    ScalarU32,
    ScalarI32,
    ScalarF32,
    ScalarU64,
    ScalarI64,
    ScalarF64,
    Text,       // String
    VecBool,
    VecScalar(Box<CapnpFieldClass>),
    VecString,
    VecStruct,  // Vec<T> where T is a struct
    Struct,     // Nested struct (pointer)
}

fn classify_type(ty: &Type) -> CapnpFieldClass {
    let ty_str = quote!(#ty).to_string().replace(' ', "");
    match ty_str.as_str() {
        "bool" => CapnpFieldClass::Bool,
        "u8" => CapnpFieldClass::ScalarU8,
        "i8" => CapnpFieldClass::ScalarI8,
        "u16" => CapnpFieldClass::ScalarU16,
        "i16" => CapnpFieldClass::ScalarI16,
        "u32" => CapnpFieldClass::ScalarU32,
        "i32" => CapnpFieldClass::ScalarI32,
        "f32" => CapnpFieldClass::ScalarF32,
        "u64" => CapnpFieldClass::ScalarU64,
        "i64" => CapnpFieldClass::ScalarI64,
        "f64" => CapnpFieldClass::ScalarF64,
        "String" => CapnpFieldClass::Text,
        _ => {
            // Check for Vec<T>
            if let Type::Path(tp) = ty {
                let seg = tp.path.segments.last().unwrap();
                if seg.ident == "Vec" {
                    if let syn::PathArguments::AngleBracketed(args) = &seg.arguments {
                        if let Some(syn::GenericArgument::Type(inner)) = args.args.first() {
                            let inner_str = quote!(#inner).to_string().replace(' ', "");
                            return match inner_str.as_str() {
                                "bool" => CapnpFieldClass::VecBool,
                                "u8" | "i8" => CapnpFieldClass::VecScalar(Box::new(classify_type(inner))),
                                "u16" | "i16" => CapnpFieldClass::VecScalar(Box::new(classify_type(inner))),
                                "u32" | "i32" | "f32" => CapnpFieldClass::VecScalar(Box::new(classify_type(inner))),
                                "u64" | "i64" | "f64" => CapnpFieldClass::VecScalar(Box::new(classify_type(inner))),
                                "String" => CapnpFieldClass::VecString,
                                _ => CapnpFieldClass::VecStruct,
                            };
                        }
                    }
                }
            }
            CapnpFieldClass::Struct
        }
    }
}

fn member_desc_tokens(
    cls: &CapnpFieldClass,
    mod_path: &proc_macro2::TokenStream,
) -> proc_macro2::TokenStream {
    match cls {
        CapnpFieldClass::Bool => quote! { #mod_path::capnp::layout::MemberDesc::Simple(#mod_path::capnp::layout::FieldKind::Bool) },
        CapnpFieldClass::ScalarU8 | CapnpFieldClass::ScalarI8 => {
            quote! { #mod_path::capnp::layout::MemberDesc::Simple(#mod_path::capnp::layout::FieldKind::Scalar(1)) }
        }
        CapnpFieldClass::ScalarU16 | CapnpFieldClass::ScalarI16 => {
            quote! { #mod_path::capnp::layout::MemberDesc::Simple(#mod_path::capnp::layout::FieldKind::Scalar(2)) }
        }
        CapnpFieldClass::ScalarU32 | CapnpFieldClass::ScalarI32 | CapnpFieldClass::ScalarF32 => {
            quote! { #mod_path::capnp::layout::MemberDesc::Simple(#mod_path::capnp::layout::FieldKind::Scalar(4)) }
        }
        CapnpFieldClass::ScalarU64 | CapnpFieldClass::ScalarI64 | CapnpFieldClass::ScalarF64 => {
            quote! { #mod_path::capnp::layout::MemberDesc::Simple(#mod_path::capnp::layout::FieldKind::Scalar(8)) }
        }
        CapnpFieldClass::Text
        | CapnpFieldClass::VecBool
        | CapnpFieldClass::VecScalar(_)
        | CapnpFieldClass::VecString
        | CapnpFieldClass::VecStruct
        | CapnpFieldClass::Struct => {
            quote! { #mod_path::capnp::layout::MemberDesc::Simple(#mod_path::capnp::layout::FieldKind::Pointer) }
        }
    }
}

fn pack_field_tokens(
    cls: &CapnpFieldClass,
    mod_path: &proc_macro2::TokenStream,
    field_expr: &proc_macro2::TokenStream,
    idx: usize,
) -> proc_macro2::TokenStream {
    let idx_lit = syn::Index::from(idx);
    match cls {
        CapnpFieldClass::Bool => {
            quote! {
                #mod_path::capnp::pack::pack_bool_field(buf, data_start, &layout.fields[#idx_lit], #field_expr);
            }
        }
        CapnpFieldClass::ScalarU8 | CapnpFieldClass::ScalarI8 |
        CapnpFieldClass::ScalarU16 | CapnpFieldClass::ScalarI16 |
        CapnpFieldClass::ScalarU32 | CapnpFieldClass::ScalarI32 | CapnpFieldClass::ScalarF32 |
        CapnpFieldClass::ScalarU64 | CapnpFieldClass::ScalarI64 | CapnpFieldClass::ScalarF64 => {
            quote! {
                #mod_path::capnp::pack::pack_data_field(buf, data_start, &layout.fields[#idx_lit], #field_expr);
            }
        }
        CapnpFieldClass::Text => {
            quote! {
                #mod_path::capnp::pack::pack_text_field(buf, ptrs_start, &layout.fields[#idx_lit], &#field_expr);
            }
        }
        CapnpFieldClass::VecBool => {
            quote! {
                #mod_path::capnp::pack::pack_bool_vec(buf, ptrs_start + layout.fields[#idx_lit].offset, &#field_expr);
            }
        }
        CapnpFieldClass::VecScalar(inner) => {
            let byte_size = match inner.as_ref() {
                CapnpFieldClass::ScalarU8 | CapnpFieldClass::ScalarI8 => quote!(1u32),
                CapnpFieldClass::ScalarU16 | CapnpFieldClass::ScalarI16 => quote!(2u32),
                CapnpFieldClass::ScalarU32 | CapnpFieldClass::ScalarI32 | CapnpFieldClass::ScalarF32 => quote!(4u32),
                CapnpFieldClass::ScalarU64 | CapnpFieldClass::ScalarI64 | CapnpFieldClass::ScalarF64 => quote!(8u32),
                _ => unreachable!(),
            };
            quote! {
                #mod_path::capnp::pack::pack_scalar_vec(buf, ptrs_start + layout.fields[#idx_lit].offset, &#field_expr, #byte_size);
            }
        }
        CapnpFieldClass::VecString => {
            quote! {
                #mod_path::capnp::pack::pack_string_vec(buf, ptrs_start + layout.fields[#idx_lit].offset, &#field_expr);
            }
        }
        CapnpFieldClass::VecStruct => {
            quote! {
                #mod_path::capnp::pack::pack_struct_vec(buf, ptrs_start + layout.fields[#idx_lit].offset, &#field_expr);
            }
        }
        CapnpFieldClass::Struct => {
            quote! {
                #mod_path::capnp::pack::pack_struct_field(buf, ptrs_start, &layout.fields[#idx_lit], &#field_expr);
            }
        }
    }
}

fn unpack_field_tokens(
    cls: &CapnpFieldClass,
    mod_path: &proc_macro2::TokenStream,
    idx: usize,
    field_ty: &Type,
) -> proc_macro2::TokenStream {
    let idx_lit = syn::Index::from(idx);
    match cls {
        CapnpFieldClass::Bool => {
            quote! { #mod_path::capnp::unpack::unpack_bool(&view, &layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarU8 => {
            quote! { #mod_path::capnp::unpack::unpack_u8(&view, &layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarI8 => {
            quote! { #mod_path::capnp::unpack::unpack_i8(&view, &layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarU16 => {
            quote! { #mod_path::capnp::unpack::unpack_u16(&view, &layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarI16 => {
            quote! { #mod_path::capnp::unpack::unpack_i16(&view, &layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarU32 => {
            quote! { #mod_path::capnp::unpack::unpack_u32(&view, &layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarI32 => {
            quote! { #mod_path::capnp::unpack::unpack_i32(&view, &layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarF32 => {
            quote! { #mod_path::capnp::unpack::unpack_f32(&view, &layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarU64 => {
            quote! { #mod_path::capnp::unpack::unpack_u64(&view, &layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarI64 => {
            quote! { #mod_path::capnp::unpack::unpack_i64(&view, &layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarF64 => {
            quote! { #mod_path::capnp::unpack::unpack_f64(&view, &layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::Text => {
            quote! { #mod_path::capnp::unpack::unpack_string(&view, &layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::VecBool => {
            quote! { #mod_path::capnp::unpack::unpack_bool_vec(&view, &layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::VecScalar(inner) => {
            let (elem_size, read_fn) = match inner.as_ref() {
                CapnpFieldClass::ScalarU8 => (quote!(1usize), quote!(#mod_path::capnp::unpack::read_u8_at)),
                CapnpFieldClass::ScalarI8 => (quote!(1usize), quote!(#mod_path::capnp::unpack::read_i8_at)),
                CapnpFieldClass::ScalarU16 => (quote!(2usize), quote!(#mod_path::capnp::unpack::read_u16_at)),
                CapnpFieldClass::ScalarI16 => (quote!(2usize), quote!(#mod_path::capnp::unpack::read_i16_at)),
                CapnpFieldClass::ScalarU32 => (quote!(4usize), quote!(#mod_path::capnp::unpack::read_u32_at)),
                CapnpFieldClass::ScalarI32 => (quote!(4usize), quote!(#mod_path::capnp::unpack::read_i32_at)),
                CapnpFieldClass::ScalarF32 => (quote!(4usize), quote!(#mod_path::capnp::unpack::read_f32_at)),
                CapnpFieldClass::ScalarU64 => (quote!(8usize), quote!(#mod_path::capnp::unpack::read_u64_at)),
                CapnpFieldClass::ScalarI64 => (quote!(8usize), quote!(#mod_path::capnp::unpack::read_i64_at)),
                CapnpFieldClass::ScalarF64 => (quote!(8usize), quote!(#mod_path::capnp::unpack::read_f64_at)),
                _ => unreachable!(),
            };
            quote! { #mod_path::capnp::unpack::unpack_scalar_vec(&view, &layout.fields[#idx_lit], #elem_size, #read_fn) }
        }
        CapnpFieldClass::VecString => {
            quote! { #mod_path::capnp::unpack::unpack_string_vec(&view, &layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::VecStruct => {
            // Extract the inner type from Vec<T>
            let inner_ty = if let Type::Path(tp) = field_ty {
                let seg = tp.path.segments.last().unwrap();
                if let syn::PathArguments::AngleBracketed(args) = &seg.arguments {
                    if let Some(syn::GenericArgument::Type(inner)) = args.args.first() {
                        quote!(#inner)
                    } else {
                        quote!(#field_ty)
                    }
                } else {
                    quote!(#field_ty)
                }
            } else {
                quote!(#field_ty)
            };
            quote! { #mod_path::capnp::unpack::unpack_struct_vec::<#inner_ty>(&view, &layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::Struct => {
            quote! { #mod_path::capnp::unpack::unpack_struct::<#field_ty>(&view, &layout.fields[#idx_lit]) }
        }
    }
}

// ── Entry points ────────────────────────────────────────────────────────

pub fn capnp_pack_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    let opts = match Options::from_derive_input(&input) {
        Ok(val) => val,
        Err(err) => return err.write_errors().into(),
    };
    let mod_path = proc_macro2::TokenStream::from_str(&opts.fracpack_mod).unwrap();

    match &input.data {
        Data::Struct(data) => {
            if let Fields::Named(named) = &data.fields {
                gen_capnp_pack(&mod_path, &input, named)
            } else {
                panic!("CapnpPack only supports named structs");
            }
        }
        _ => panic!("CapnpPack only supports structs"),
    }
}

pub fn capnp_unpack_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    let opts = match Options::from_derive_input(&input) {
        Ok(val) => val,
        Err(err) => return err.write_errors().into(),
    };
    let mod_path = proc_macro2::TokenStream::from_str(&opts.fracpack_mod).unwrap();

    match &input.data {
        Data::Struct(data) => {
            if let Fields::Named(named) = &data.fields {
                gen_capnp_unpack(&mod_path, &input, named)
            } else {
                panic!("CapnpUnpack only supports named structs");
            }
        }
        _ => panic!("CapnpUnpack only supports structs"),
    }
}

// ── Code generation ─────────────────────────────────────────────────────

fn gen_capnp_pack(
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
        (ident, ty, cls)
    }).collect();

    let member_descs: Vec<_> = field_infos.iter().map(|(_, _, cls)| {
        member_desc_tokens(cls, mod_path)
    }).collect();

    let pack_stmts: Vec<_> = field_infos.iter().enumerate().map(|(i, (ident, _, cls))| {
        let field_expr = quote!(self.#ident);
        pack_field_tokens(cls, mod_path, &field_expr, i)
    }).collect();

    let expanded = quote! {
        impl #impl_generics #mod_path::capnp::pack::CapnpPack for #name #ty_generics #where_clause {
            fn member_descs() -> Vec<#mod_path::capnp::layout::MemberDesc> {
                vec![
                    #(#member_descs),*
                ]
            }

            fn pack_into(
                &self,
                buf: &mut #mod_path::capnp::pack::WordBuf,
                data_start: u32,
                ptrs_start: u32,
            ) {
                let layout = Self::capnp_layout();
                #(#pack_stmts)*
            }
        }
    };
    expanded.into()
}

fn gen_capnp_unpack(
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
        (ident, ty, cls)
    }).collect();

    // For unpack we need member_descs to compute the layout at runtime
    let member_descs: Vec<_> = field_infos.iter().map(|(_, _, cls)| {
        member_desc_tokens(cls, mod_path)
    }).collect();

    let field_inits: Vec<_> = field_infos.iter().enumerate().map(|(i, (ident, ty, cls))| {
        let unpack_expr = unpack_field_tokens(cls, mod_path, i, ty);
        quote! { #ident: #unpack_expr }
    }).collect();

    let expanded = quote! {
        impl #impl_generics #mod_path::capnp::unpack::CapnpUnpack for #name #ty_generics #where_clause {
            fn unpack_from(view: &#mod_path::capnp::view::CapnpView) -> Self {
                let descs: Vec<#mod_path::capnp::layout::MemberDesc> = vec![
                    #(#member_descs),*
                ];
                let layout = #mod_path::capnp::layout::CapnpLayout::compute(&descs);
                #name {
                    #(#field_inits),*
                }
            }
        }
    };
    expanded.into()
}

// ── CapnpView derive macro ─────────────────────────────────────────────

pub fn capnp_view_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    let opts = match Options::from_derive_input(&input) {
        Ok(val) => val,
        Err(err) => return err.write_errors().into(),
    };
    let mod_path = proc_macro2::TokenStream::from_str(&opts.fracpack_mod).unwrap();

    match &input.data {
        Data::Struct(data) => {
            if let Fields::Named(named) = &data.fields {
                gen_capnp_view(&mod_path, &input, named)
            } else {
                panic!("CapnpView only supports named structs");
            }
        }
        _ => panic!("CapnpView only supports structs"),
    }
}

/// Generate the view accessor expression for a field.
fn view_field_tokens(
    cls: &CapnpFieldClass,
    _mod_path: &proc_macro2::TokenStream,
    idx: usize,
) -> proc_macro2::TokenStream {
    let idx_lit = syn::Index::from(idx);
    match cls {
        CapnpFieldClass::Bool => {
            quote! { self.view.read_bool(&layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarU8 => {
            quote! { self.view.read_u8(&layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarI8 => {
            quote! { self.view.read_i8(&layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarU16 => {
            quote! { self.view.read_u16(&layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarI16 => {
            quote! { self.view.read_i16(&layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarU32 => {
            quote! { self.view.read_u32(&layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarI32 => {
            quote! { self.view.read_i32(&layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarF32 => {
            quote! { self.view.read_f32(&layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarU64 => {
            quote! { self.view.read_u64(&layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarI64 => {
            quote! { self.view.read_i64(&layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::ScalarF64 => {
            quote! { self.view.read_f64(&layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::Text => {
            quote! { self.view.read_text(&layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::Struct => {
            quote! { self.view.read_struct(&layout.fields[#idx_lit]) }
        }
        CapnpFieldClass::VecBool
        | CapnpFieldClass::VecScalar(_)
        | CapnpFieldClass::VecString
        | CapnpFieldClass::VecStruct => {
            quote! { self.view.read_list(&layout.fields[#idx_lit]) }
        }
    }
}

/// Return type for a view accessor.
fn view_return_type(
    cls: &CapnpFieldClass,
    mod_path: &proc_macro2::TokenStream,
) -> proc_macro2::TokenStream {
    match cls {
        CapnpFieldClass::Bool => quote! { bool },
        CapnpFieldClass::ScalarU8 => quote! { u8 },
        CapnpFieldClass::ScalarI8 => quote! { i8 },
        CapnpFieldClass::ScalarU16 => quote! { u16 },
        CapnpFieldClass::ScalarI16 => quote! { i16 },
        CapnpFieldClass::ScalarU32 => quote! { u32 },
        CapnpFieldClass::ScalarI32 => quote! { i32 },
        CapnpFieldClass::ScalarF32 => quote! { f32 },
        CapnpFieldClass::ScalarU64 => quote! { u64 },
        CapnpFieldClass::ScalarI64 => quote! { i64 },
        CapnpFieldClass::ScalarF64 => quote! { f64 },
        CapnpFieldClass::Text => quote! { &'a str },
        CapnpFieldClass::Struct => quote! { #mod_path::capnp::view::CapnpView<'a> },
        CapnpFieldClass::VecBool
        | CapnpFieldClass::VecScalar(_)
        | CapnpFieldClass::VecString
        | CapnpFieldClass::VecStruct => {
            quote! { Option<(&'a [u8], #mod_path::capnp::wire::ListInfo)> }
        }
    }
}

fn gen_capnp_view(
    mod_path: &proc_macro2::TokenStream,
    input: &DeriveInput,
    named: &syn::FieldsNamed,
) -> TokenStream {
    let name = &input.ident;
    let vis = &input.vis;
    let view_name = syn::Ident::new(&format!("{}CapnpView", name), name.span());

    let field_infos: Vec<_> = named.named.iter().map(|f| {
        let ident = f.ident.as_ref().unwrap();
        let ty = &f.ty;
        let cls = classify_type(ty);
        (ident, ty, cls)
    }).collect();

    let member_descs: Vec<_> = field_infos.iter().map(|(_, _, cls)| {
        member_desc_tokens(cls, mod_path)
    }).collect();

    let field_accessors: Vec<_> = field_infos.iter().enumerate().map(|(i, (ident, _, cls))| {
        let ret_ty = view_return_type(cls, mod_path);
        let expr = view_field_tokens(cls, mod_path, i);
        quote! {
            pub fn #ident(&self) -> #ret_ty {
                let descs: Vec<#mod_path::capnp::layout::MemberDesc> = vec![
                    #(#member_descs),*
                ];
                let layout = #mod_path::capnp::layout::CapnpLayout::compute(&descs);
                #expr
            }
        }
    }).collect();

    let expanded = quote! {
        #[derive(Debug, Clone, Copy)]
        #vis struct #view_name<'a> {
            view: #mod_path::capnp::view::CapnpView<'a>,
        }

        impl<'a> #view_name<'a> {
            /// Create a view over a complete Cap'n Proto message.
            pub fn from_message(data: &'a [u8]) -> Result<Self, &'static str> {
                let view = #mod_path::capnp::view::CapnpView::from_message(data)?;
                Ok(#view_name { view })
            }

            /// Create a view from an existing CapnpView.
            pub fn from_view(view: #mod_path::capnp::view::CapnpView<'a>) -> Self {
                #view_name { view }
            }

            #(#field_accessors)*
        }
    };
    expanded.into()
}
