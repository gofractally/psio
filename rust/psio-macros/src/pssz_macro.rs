//! Explicit PSSZ derives. These never generate legacy fracpack implementations.
use proc_macro::TokenStream;
use proc_macro2::TokenStream as Tokens;
use quote::{format_ident, quote};
use syn::{
    parse_macro_input, parse_quote, Data, DeriveInput, Fields, Lit, Member, Meta, NestedMeta, Type,
};

struct Options {
    krate: syn::Path,
    dwnc: bool,
    transparent: bool,
    name: Option<String>,
}
fn options(input: &DeriveInput) -> syn::Result<Options> {
    for param in &input.generics.params {
        let reserved = match param {
            syn::GenericParam::Type(t) => t.ident == "__PsszFormat",
            syn::GenericParam::Const(c) => c.ident == "__PsszFormat",
            syn::GenericParam::Lifetime(l) => l.lifetime.ident == "__pssz_view",
        };
        if reserved { return Err(syn::Error::new_spanned(param, "generic name is reserved by PSSZ derives")); }
    }
    let mut out = Options {
        krate: parse_quote!(::psio),
        dwnc: false,
        transparent: false,
        name: None,
    };
    for attr in &input.attrs {
        if !attr.path.is_ident("pssz") {
            continue;
        }
        let list = match attr.parse_meta()? {
            Meta::List(list) => list,
            other => return Err(syn::Error::new_spanned(other, "expected #[pssz(...)]")),
        };
        for item in list.nested {
            match item {
                NestedMeta::Meta(Meta::Path(path))
                    if path.is_ident("definition_will_not_change") =>
                {
                    out.dwnc = true
                }
                NestedMeta::Meta(Meta::Path(path)) if path.is_ident("transparent") => {
                    out.transparent = true
                }
                NestedMeta::Meta(Meta::NameValue(nv)) if nv.path.is_ident("crate") => {
                    if let Lit::Str(value) = nv.lit {
                        out.krate = value.parse()?;
                    } else {
                        return Err(syn::Error::new_spanned(nv, "crate must be a path string"));
                    }
                }
                NestedMeta::Meta(Meta::NameValue(nv)) if nv.path.is_ident("name") => {
                    if let Lit::Str(value) = nv.lit {
                        out.name = Some(value.value());
                    } else {
                        return Err(syn::Error::new_spanned(nv, "name must be a string"));
                    }
                }
                other => return Err(syn::Error::new_spanned(other, "unsupported PSSZ attribute")),
            }
        }
    }
    if out.transparent && out.dwnc {
        return Err(syn::Error::new_spanned(
            input,
            "transparent and definition_will_not_change cannot be combined",
        ));
    }
    let fields: Vec<_> = match &input.data {
        Data::Struct(s) => s.fields.iter().collect(),
        Data::Enum(e) => {
            for variant in &e.variants {
                if variant.attrs.iter().any(|a| a.path.is_ident("pssz")) {
                    return Err(syn::Error::new_spanned(
                        variant,
                        "variant PSSZ annotations are unsupported; use explicit payload types",
                    ));
                }
            }
            e.variants.iter().flat_map(|v| v.fields.iter()).collect()
        }
        Data::Union(u) => u.fields.named.iter().collect(),
    };
    for field in fields {
        if field.attrs.iter().any(|a| a.path.is_ident("pssz")) {
            return Err(syn::Error::new_spanned(
                field,
                "field PSSZ annotations are unsupported; use bounded wrapper types",
            ));
        }
    }
    Ok(out)
}

fn field_info(fields: &Fields) -> Vec<(Member, &Type)> {
    fields
        .iter()
        .enumerate()
        .map(|(i, f)| {
            (
                f.ident
                    .clone()
                    .map(Member::Named)
                    .unwrap_or_else(|| Member::Unnamed(i.into())),
                &f.ty,
            )
        })
        .collect()
}

fn generic_fields(input: &DeriveInput) -> Vec<&Type> {
    let names: Vec<_> = input
        .generics
        .type_params()
        .map(|p| p.ident.to_string())
        .collect();
    let own_name = input.ident.to_string();
    let fields: Vec<_> = match &input.data {
        Data::Struct(s) => s.fields.iter().collect(),
        Data::Enum(e) => e.variants.iter().flat_map(|v| v.fields.iter()).collect(),
        Data::Union(u) => u.fields.named.iter().collect(),
    };
    fields
        .into_iter()
        .filter_map(|field| {
            let ty = &field.ty;
            let text = quote!(#ty).to_string();
            let parts: Vec<_> = text
                .split(|c: char| !c.is_alphanumeric() && c != '_')
                .collect();
            if !parts.contains(&own_name.as_str())
                && names.iter().any(|n| parts.contains(&n.as_str()))
            {
                Some(ty)
            } else {
                None
            }
        })
        .collect()
}

fn record_encode(k: &syn::Path, types: &[&Type], values: &[Tokens], dwnc: bool) -> Tokens {
    let indices: Vec<_> = (0..types.len()).collect();
    quote! {
        let __keep = #k::pssz_checked::record_kept_fields(&[
            #(<#types as #k::pssz::PsszPack<__PsszFormat>>::IS_OPTIONAL && <#types as #k::pssz::PsszPack<__PsszFormat>>::pssz_is_none(#values)),*
        ], #dwnc);
        #k::pssz_checked::encode_record::<__PsszFormat>(&[
            #(#k::pssz_checked::field_if_kept::<__PsszFormat, #types>(#values, #indices < __keep)?),*
        ], #dwnc, out)
    }
}

fn record_decode(k: &syn::Path, fields: &Fields, construct: Tokens, dwnc: bool) -> Tokens {
    let info = field_info(fields);
    let types: Vec<_> = info.iter().map(|(_, ty)| *ty).collect();
    let values: Vec<_> = (0..info.len())
        .map(|i| format_ident!("__pssz_field_{i}"))
        .collect();
    let initializer = match fields {
        Fields::Named(_) => {
            let names: Vec<_> = info.iter().map(|(m, _)| m).collect();
            quote! { #construct { #(#names: #values),* } }
        }
        Fields::Unnamed(_) => quote! { #construct ( #(#values),* ) },
        Fields::Unit => construct,
    };
    quote! {
        let __spans = #k::pssz_checked::record_spans::<__PsszFormat>(&[
            #(#k::pssz_checked::Layout::of::<__PsszFormat, #types>()),*
        ], bytes, #dwnc)?;
        let mut __spans = __spans.into_iter();
        #(let #values = #k::pssz_checked::decode_field::<__PsszFormat, #types>(bytes,
            __spans.next().expect("derived field count"))?;)*
        ::core::result::Result::Ok(#initializer)
    }
}

fn record_validate(k: &syn::Path, fields: &Fields, dwnc: bool) -> Tokens {
    let info = field_info(fields);
    let types: Vec<_> = info.iter().map(|(_, ty)| *ty).collect();
    quote! {
        let __spans = #k::pssz_checked::record_spans::<__PsszFormat>(&[
            #(#k::pssz_checked::Layout::of::<__PsszFormat, #types>()),*
        ], bytes, #dwnc)?;
        let mut __spans = __spans.into_iter();
        #(if let ::core::option::Option::Some(span) = __spans.next().expect("derived field count") {
            <#types as #k::pssz::PsszValidate<__PsszFormat>>::pssz_validate(&bytes[span])?;
        })*
        ::core::result::Result::Ok(())
    }
}

fn expand(input: DeriveInput, unpack: bool) -> syn::Result<Tokens> {
    let opts = options(&input)?;
    let k = &opts.krate;
    let name = &input.ident;
    if input
        .generics
        .params
        .iter()
        .any(|p| matches!(p, syn::GenericParam::Type(t) if t.ident == "__PsszFormat"))
    {
        return Err(syn::Error::new_spanned(
            input,
            "__PsszFormat is reserved by PSSZ derives",
        ));
    }
    let mut generics = input.generics.clone();
    generics
        .params
        .push(parse_quote!(__PsszFormat: #k::pssz::PsszFormat));
    for ty in generic_fields(&input) {
        generics.make_where_clause().predicates.push(if unpack {
            parse_quote!(#ty: #k::pssz::PsszPack<__PsszFormat> + #k::pssz::PsszUnpack<__PsszFormat>)
        } else {
            parse_quote!(#ty: #k::pssz::PsszPack<__PsszFormat>)
        });
    }
    let (impl_g, _, where_c) = generics.split_for_impl();
    let mut validation_generics = input.generics.clone();
    validation_generics
        .params
        .push(parse_quote!(__PsszFormat: #k::pssz::PsszFormat));
    for ty in generic_fields(&input) {
        validation_generics.make_where_clause().predicates.push(parse_quote!(#ty: #k::pssz::PsszPack<__PsszFormat> + #k::pssz::PsszValidate<__PsszFormat>));
    }
    let (validation_g, _, validation_where) = validation_generics.split_for_impl();
    let (_, type_g, _) = input.generics.split_for_impl();
    let (body, fixed, size, min, max) = match &input.data {
        Data::Struct(data) => {
            let info = field_info(&data.fields);
            let types: Vec<_> = info.iter().map(|(_, ty)| *ty).collect();
            let members: Vec<_> = info.iter().map(|(m, _)| m).collect();
            if opts.transparent {
                if info.len() != 1 {
                    return Err(syn::Error::new_spanned(
                        &data.fields,
                        "transparent PSSZ structs require exactly one field",
                    ));
                }
                let ty = types[0];
                let member = members[0];
                let decode = match &data.fields {
                    Fields::Named(_) => {
                        quote! { ::core::result::Result::Ok(Self { #member: <#ty as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_unpack(bytes)? }) }
                    }
                    _ => {
                        quote! { ::core::result::Result::Ok(Self(<#ty as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_unpack(bytes)?)) }
                    }
                };
                (
                    if unpack {
                        decode
                    } else {
                        quote! { <#ty as #k::pssz::PsszPack<__PsszFormat>>::pssz_pack_checked(&self.#member, out) }
                    },
                    quote!(<#ty as #k::pssz::PsszPack<__PsszFormat>>::IS_FIXED_SIZE),
                    quote!(<#ty as #k::pssz::PsszPack<__PsszFormat>>::FIXED_SIZE),
                    quote!(<#ty as #k::pssz::PsszPack<__PsszFormat>>::MIN_ENCODED_SIZE),
                    quote!(<#ty as #k::pssz::PsszPack<__PsszFormat>>::MAX_ENCODED_SIZE),
                )
            } else {
                let dwnc = opts.dwnc;
                let values = members.iter().map(|m| quote!(&self.#m)).collect::<Vec<_>>();
                (
                    if unpack {
                        record_decode(k, &data.fields, quote!(Self), dwnc)
                    } else {
                        record_encode(k, &types, &values, dwnc)
                    },
                    quote!(#dwnc #(&& <#types as #k::pssz::PsszPack<__PsszFormat>>::IS_FIXED_SIZE)*),
                    quote!(0usize #(.saturating_add(<#types as #k::pssz::PsszPack<__PsszFormat>>::FIXED_SIZE))*),
                    if dwnc {
                        quote!(0usize #(.saturating_add(if <#types as #k::pssz::PsszPack<__PsszFormat>>::IS_FIXED_SIZE { <#types as #k::pssz::PsszPack<__PsszFormat>>::FIXED_SIZE } else { __PsszFormat::OFFSET_BYTES }))*)
                    } else {
                        quote!(__PsszFormat::HEADER_BYTES)
                    },
                    quote!(::core::option::Option::None),
                )
            }
        }
        Data::Enum(data) => {
            if opts.transparent || opts.dwnc {
                return Err(syn::Error::new_spanned(
                    input,
                    "PSSZ enum selectors cannot be transparent or DWNC",
                ));
            }
            if data.variants.is_empty() || data.variants.len() > 256 {
                return Err(syn::Error::new_spanned(
                    data.enum_token,
                    "PSSZ enums require 1..=256 variants",
                ));
            }
            let mut arms = Vec::new();
            for (index, variant) in data.variants.iter().enumerate() {
                if variant.discriminant.is_some() {
                    return Err(syn::Error::new_spanned(variant, "PSSZ selectors use declaration order; explicit discriminants are unsupported"));
                }
                let vname = &variant.ident;
                let info = field_info(&variant.fields);
                let types: Vec<_> = info.iter().map(|(_, ty)| *ty).collect();
                let bindings: Vec<_> = (0..info.len())
                    .map(|i| format_ident!("__pssz_value_{i}"))
                    .collect();
                let tag = index as u8;
                if unpack {
                    let decode = match &variant.fields {
                        Fields::Unit => {
                            quote! { let _payload_depth = #k::pssz_checked::DecodeDepth::enter()?; if !bytes.is_empty() { return ::core::result::Result::Err(#k::pssz::PsszError("pssz: trailing unit variant bytes")); } ::core::result::Result::Ok(Self::#vname) }
                        }
                        Fields::Unnamed(_) if info.len() == 1 => {
                            let ty = types[0];
                            quote! { ::core::result::Result::Ok(Self::#vname(<#ty as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_unpack(bytes)?)) }
                        }
                        _ => {
                            let body =
                                record_decode(k, &variant.fields, quote!(Self::#vname), false);
                            quote! { let _payload_depth = #k::pssz_checked::DecodeDepth::enter()?; #body }
                        }
                    };
                    arms.push(quote!(#tag => { #decode }));
                } else {
                    let pattern = match &variant.fields {
                        Fields::Unit => quote!(Self::#vname),
                        Fields::Unnamed(_) => quote!(Self::#vname(#(#bindings),*)),
                        Fields::Named(_) => {
                            let members: Vec<_> = info.iter().map(|(m, _)| m).collect();
                            quote!(Self::#vname { #(#members: #bindings),* })
                        }
                    };
                    let encode = match &variant.fields {
                        Fields::Unit => {
                            quote! { let _payload_depth = #k::pssz_checked::EncodeDepth::enter()?; ::core::result::Result::Ok(()) }
                        }
                        Fields::Unnamed(_) if info.len() == 1 => {
                            let ty = types[0];
                            let b = &bindings[0];
                            quote!(<#ty as #k::pssz::PsszPack<__PsszFormat>>::pssz_pack_checked(#b, out))
                        }
                        _ => {
                            let body = record_encode(
                                k,
                                &types,
                                &bindings.iter().map(|b| quote!(#b)).collect::<Vec<_>>(),
                                false,
                            );
                            quote! { let _payload_depth = #k::pssz_checked::EncodeDepth::enter()?; #body }
                        }
                    };
                    arms.push(quote!(#pattern => { out.push(#tag); #encode }));
                }
            }
            let body = if unpack {
                quote! {
                    let (&selector, bytes) = bytes.split_first().ok_or(#k::pssz::PsszError("pssz: enum selector missing"))?;
                    match selector { #(#arms,)* _ => ::core::result::Result::Err(#k::pssz::PsszError("pssz: invalid enum selector")) }
                }
            } else {
                quote! { match self { #(#arms),* } }
            };
            (
                body,
                quote!(false),
                quote!(0),
                quote!(1),
                quote!(::core::option::Option::None),
            )
        }
        Data::Union(data) => {
            return Err(syn::Error::new_spanned(
                data.union_token,
                "PSSZ does not support Rust unions",
            ))
        }
    };
    let depth_guard = if opts.transparent {
        quote!()
    } else {
        quote!(let _depth = #k::pssz_checked::DecodeDepth::enter()?;)
    };
    let encode_depth_guard = if opts.transparent {
        quote!()
    } else {
        quote!(let _depth = #k::pssz_checked::EncodeDepth::enter()?;)
    };
    let validation_body = match &input.data {
        Data::Struct(data) if opts.transparent => {
            let ty = &data.fields.iter().next().unwrap().ty;
            quote! { <#ty as #k::pssz::PsszValidate<__PsszFormat>>::pssz_validate(bytes) }
        }
        Data::Struct(data) => record_validate(k, &data.fields, opts.dwnc),
        Data::Enum(data) => {
            let arms = data.variants.iter().enumerate().map(|(index, variant)| {
                let tag = index as u8;
                let body = match &variant.fields {
                    Fields::Unit => quote! { let _payload = #k::pssz_checked::DecodeDepth::enter()?; if bytes.is_empty() { ::core::result::Result::Ok(()) } else { ::core::result::Result::Err(#k::pssz::PsszError("pssz: trailing unit variant bytes")) } },
                    Fields::Unnamed(fields) if fields.unnamed.len() == 1 => { let ty = &fields.unnamed[0].ty; quote! { <#ty as #k::pssz::PsszValidate<__PsszFormat>>::pssz_validate(bytes) } },
                    _ => { let body = record_validate(k, &variant.fields, false); quote! { let _payload = #k::pssz_checked::DecodeDepth::enter()?; #body } },
                };
                quote!(#tag => { #body })
            });
            quote! {
                let (&selector, bytes) = bytes.split_first().ok_or(#k::pssz::PsszError("pssz: enum selector missing"))?;
                match selector { #(#arms,)* _ => ::core::result::Result::Err(#k::pssz::PsszError("pssz: invalid enum selector")) }
            }
        }
        _ => {
            quote!(::core::result::Result::Err(#k::pssz::PsszError("pssz: unsupported validation type")))
        }
    };
    let mut optional_metadata = quote!();
    let mut missing_optional = quote!();
    if opts.transparent {
        if let Data::Struct(data) = &input.data {
            let info = field_info(&data.fields);
            let (member, ty) = &info[0];
            optional_metadata = quote! {
                const IS_OPTIONAL: bool = <#ty as #k::pssz::PsszPack<__PsszFormat>>::IS_OPTIONAL;
                fn pssz_is_none(&self) -> bool { <#ty as #k::pssz::PsszPack<__PsszFormat>>::pssz_is_none(&self.#member) }
                fn pssz_none_bytes() -> ::core::option::Option<&'static [u8]> { <#ty as #k::pssz::PsszPack<__PsszFormat>>::pssz_none_bytes() }
            };
            let value =
                quote!(<#ty as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_missing_optional()?);
            let construct = if matches!(&data.fields, Fields::Named(_)) {
                quote!(Self { #member: #value })
            } else {
                quote!(Self(#value))
            };
            missing_optional = quote! { fn pssz_missing_optional() -> #k::pssz::PsszResult<Self> { ::core::result::Result::Ok(#construct) } };
        }
    }
    Ok(if unpack {
        quote! {
            impl #impl_g #k::pssz::PsszUnpack<__PsszFormat> for #name #type_g #where_c {
                #missing_optional
                fn pssz_unpack(bytes: &[u8]) -> #k::pssz::PsszResult<Self> {
                    #depth_guard
                    #body
                }
            }
            impl #validation_g #k::pssz::PsszValidate<__PsszFormat> for #name #type_g #validation_where {
                fn pssz_validate(bytes: &[u8]) -> #k::pssz::PsszResult<()> {
                    #depth_guard
                    #validation_body
                }
            }
        }
    } else {
        quote! {
            impl #impl_g #k::pssz::PsszPack<__PsszFormat> for #name #type_g #where_c {
                #optional_metadata
                const MIN_ENCODED_SIZE: usize = #min;
                const MAX_ENCODED_SIZE: ::core::option::Option<usize> = #max;
                const IS_FIXED_SIZE: bool = #fixed;
                const FIXED_SIZE: usize = if <Self as #k::pssz::PsszPack<__PsszFormat>>::IS_FIXED_SIZE { #size } else { 0 };
                fn pssz_size(&self) -> usize { #k::pssz::try_to_pssz::<__PsszFormat, _>(self).expect("pssz: encode size failed").len() }
                fn pssz_pack(&self, out: &mut ::std::vec::Vec<u8>) { <Self as #k::pssz::PsszPack<__PsszFormat>>::pssz_pack_checked(self, out).expect("pssz: encode failed; use try_to_pssz"); }
                fn pssz_pack_checked(&self, destination: &mut ::std::vec::Vec<u8>) -> #k::pssz::PsszResult<()> {
                    #encode_depth_guard
                    let mut temporary = ::std::vec::Vec::new();
                    let out = &mut temporary;
                    let result: #k::pssz::PsszResult<()> = (|| { #body })();
                    result?;
                    destination.try_reserve(temporary.len()).map_err(|_| #k::pssz::PsszError("pssz: output allocation overflow"))?;
                    destination.extend_from_slice(&temporary);
                    ::core::result::Result::Ok(())
                }
            }
        }
    })
}

pub fn derive(input: TokenStream, unpack: bool) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    expand(input, unpack)
        .unwrap_or_else(syn::Error::into_compile_error)
        .into()
}

fn schema_record(k: &syn::Path, fields: &Fields, dwnc: bool) -> Tokens {
    let info = field_info(fields);
    let names: Vec<_> = info
        .iter()
        .map(|(m, _)| match m {
            Member::Named(id) => id.to_string().trim_start_matches("r#").to_string(),
            Member::Unnamed(index) => index.index.to_string(),
        })
        .collect();
    let types: Vec<_> = info.iter().map(|(_, ty)| *ty).collect();
    let attributes = if dwnc {
        quote! { ::std::vec![#k::pssz::schema_ir::Attribute { name: "definitionWillNotChange".into(), value: ::core::option::Option::Some(::std::string::String::new()) }] }
    } else {
        quote!(::std::vec![])
    };
    quote! {
        #k::pssz::schema_ir::AnyType::new(#k::pssz::schema_ir::AnyValue::Object(#k::pssz::schema_ir::Object {
            members: ::std::vec![#(#k::pssz::schema_ir::Member {
                name: #names.into(), r#type: ::std::boxed::Box::new(<#types as #k::pssz::schema_ir::ToPsszSchema>::schema(builder)?), attributes: ::std::vec![]
            }),*], attributes: #attributes
        }))
    }
}

fn expand_schema(input: DeriveInput) -> syn::Result<Tokens> {
    let opts = options(&input)?;
    let k = &opts.krate;
    let name = &input.ident;
    let mut generics = input.generics.clone();
    for param in input.generics.type_params() {
        let id = &param.ident;
        generics
            .make_where_clause()
            .predicates
            .push(parse_quote!(#id: 'static));
    }
    for ty in generic_fields(&input) {
        generics
            .make_where_clause()
            .predicates
            .push(parse_quote!(#ty: #k::pssz::schema_ir::ToPsszSchema));
    }
    let (impl_g, _, where_c) = generics.split_for_impl();
    let (_, type_g, _) = input.generics.split_for_impl();
    let body = match &input.data {
        Data::Struct(data) if opts.transparent => {
            if data.fields.len() != 1 {
                return Err(syn::Error::new_spanned(
                    &data.fields,
                    "transparent PSSZ structs require exactly one field",
                ));
            }
            let ty = &data.fields.iter().next().unwrap().ty;
            quote! { <#ty as #k::pssz::schema_ir::ToPsszSchema>::schema(builder)? }
        }
        Data::Struct(data) => schema_record(k, &data.fields, opts.dwnc),
        Data::Enum(data) => {
            if opts.dwnc || opts.transparent {
                return Err(syn::Error::new_spanned(
                    input,
                    "PSSZ enums cannot be transparent or DWNC",
                ));
            }
            if data.variants.is_empty() || data.variants.len() > 256 {
                return Err(syn::Error::new_spanned(
                    data.enum_token,
                    "PSSZ enums require 1..=256 variants",
                ));
            }
            let mut members = Vec::new();
            for variant in &data.variants {
                if variant.discriminant.is_some() {
                    return Err(syn::Error::new_spanned(
                        variant,
                        "PSSZ selectors use declaration order",
                    ));
                }
                let variant_name = variant.ident.to_string();
                let ty = match &variant.fields {
                    Fields::Unit => {
                        quote! { #k::pssz::schema_ir::AnyType::new(#k::pssz::schema_ir::AnyValue::Struct(#k::pssz::schema_ir::Struct { members: ::std::vec![], attributes: ::std::vec![] })) }
                    }
                    Fields::Unnamed(fields) if fields.unnamed.len() == 1 => {
                        let ty = &fields.unnamed[0].ty;
                        quote! { <#ty as #k::pssz::schema_ir::ToPsszSchema>::schema(builder)? }
                    }
                    Fields::Unnamed(fields) => {
                        let types: Vec<_> = fields.unnamed.iter().map(|f| &f.ty).collect();
                        quote! {
                            #k::pssz::schema_ir::AnyType::new(#k::pssz::schema_ir::AnyValue::Tuple(#k::pssz::schema_ir::Tuple { members: ::std::vec![#(::std::boxed::Box::new(<#types as #k::pssz::schema_ir::ToPsszSchema>::schema(builder)?)),*] }))
                        }
                    }
                    _ => schema_record(k, &variant.fields, false),
                };
                members.push(quote! { #k::pssz::schema_ir::Member { name: #variant_name.into(), r#type: ::std::boxed::Box::new(#ty), attributes: ::std::vec![] } });
            }
            quote! { #k::pssz::schema_ir::AnyType::new(#k::pssz::schema_ir::AnyValue::Variant(#k::pssz::schema_ir::Variant { members: ::std::vec![#(#members),*], attributes: ::std::vec![] })) }
        }
        Data::Union(data) => {
            return Err(syn::Error::new_spanned(
                data.union_token,
                "PSSZ schema does not support Rust unions",
            ))
        }
    };
    let schema_name = if let Some(name) = &opts.name {
        quote!(::core::option::Option::Some(#name.into()))
    } else if input.generics.params.is_empty() {
        let text = name.to_string();
        quote!(::core::option::Option::Some(#text.into()))
    } else {
        quote!(::core::option::Option::Some(
            ::std::any::type_name::<Self>().into()
        ))
    };
    Ok(quote! {
        impl #impl_g #k::pssz::schema_ir::ToPsszSchema for #name #type_g #where_c {
            fn schema_name() -> ::core::option::Option<::std::string::String> { #schema_name }
            fn build_schema(builder: &mut #k::pssz::schema_ir::SchemaBuilder) -> #k::pssz::PsszResult<#k::pssz::schema_ir::AnyType> { ::core::result::Result::Ok(#body) }
        }
    })
}

pub fn derive_schema(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    expand_schema(input)
        .unwrap_or_else(syn::Error::into_compile_error)
        .into()
}

fn expand_view(input: DeriveInput) -> syn::Result<Tokens> {
    let opts = options(&input)?;
    let k = &opts.krate;
    let name = &input.ident;
    let accessor = format_ident!("{}PsszAccessors", name);
    let vis = &input.vis;
    let mut generics = input.generics.clone();
    generics.params.insert(0, parse_quote!('__pssz_view));
    generics
        .params
        .push(parse_quote!(__PsszFormat: #k::pssz::PsszFormat));
    for ty in generic_fields(&input) {
        generics
            .make_where_clause()
            .predicates
            .push(parse_quote!(#ty: #k::pssz::PsszPack<__PsszFormat>));
    }
    let (impl_g, trait_g, where_c) = generics.split_for_impl();
    let (_, type_g, _) = input.generics.split_for_impl();
    let mut declarations = Vec::new();
    let mut methods = Vec::new();
    let mut field_impls = Vec::new();
    match &input.data {
        Data::Struct(data) => {
            let info = field_info(&data.fields);
            let types: Vec<_> = info.iter().map(|(_, ty)| *ty).collect();
            if opts.transparent && info.len() != 1 {
                return Err(syn::Error::new_spanned(
                    data.fields.clone(),
                    "transparent PSSZ view requires one field",
                ));
            }
            let mut field_generics = input.generics.clone();
            field_generics
                .params
                .push(parse_quote!(__PsszFormat: #k::pssz::PsszFormat));
            for ty in generic_fields(&input) {
                field_generics
                    .make_where_clause()
                    .predicates
                    .push(parse_quote!(#ty: #k::pssz::PsszPack<__PsszFormat>));
            }
            let (field_g, _, field_where) = field_generics.split_for_impl();
            for (index, (member, ty)) in info.iter().enumerate() {
                let method = match member {
                    Member::Named(id) => id.clone(),
                    Member::Unnamed(index) => format_ident!("field_{}", index.index),
                };
                declarations.push(quote!(fn #method(&self) -> #k::pssz_view::PsszView<'__pssz_view, #ty, __PsszFormat>;));
                methods.push(quote! { fn #method(&self) -> #k::pssz_view::PsszView<'__pssz_view, #ty, __PsszFormat> { self.field::<#index>() } });
                let dwnc = opts.dwnc;
                let transparent = opts.transparent;
                field_impls.push(quote! {
                    impl #field_g #k::pssz_view::PsszField<__PsszFormat, #index> for #name #type_g #field_where {
                        type Field = #ty;
                        const LOCATION: #k::pssz_checked::FieldLocation = #k::pssz_checked::record_location::<__PsszFormat>(&[
                            #(#k::pssz_checked::Layout::of::<__PsszFormat, #types>()),*
                        ], #index);
                        const DWNC: bool = #dwnc;
                        const TRANSPARENT: bool = #transparent;
                    }
                });
            }
        }
        Data::Enum(data) => {
            if opts.dwnc || opts.transparent {
                return Err(syn::Error::new_spanned(
                    input.clone(),
                    "PSSZ enum view cannot be transparent or DWNC",
                ));
            }
            if data.variants.is_empty() || data.variants.len() > 256 {
                return Err(syn::Error::new_spanned(data.enum_token, "PSSZ enums require 1..=256 variants"));
            }
            if let Some(variant) = data.variants.iter().find(|v| v.discriminant.is_some()) {
                return Err(syn::Error::new_spanned(variant, "PSSZ selectors use declaration order; explicit discriminants are unsupported"));
            }
            declarations.push(quote!(
                fn selector(&self) -> u8;
            ));
            methods.push(quote!(
                fn selector(&self) -> u8 {
                    self.data[0]
                }
            ));
            for (index, variant) in data.variants.iter().enumerate() {
                let method = format_ident!("as_{}", variant.ident);
                let info = field_info(&variant.fields);
                let types: Vec<_> = info.iter().map(|(_, ty)| *ty).collect();
                let payload = match &variant.fields {
                    Fields::Unit => quote!([u8; 0]),
                    Fields::Unnamed(_) if types.len() == 1 => {
                        let ty = types[0];
                        quote!(#ty)
                    }
                    _ => quote!((#(#types,)*)),
                };
                let tag = index as u8;
                declarations.push(quote!(fn #method(&self) -> ::core::option::Option<#k::pssz_view::PsszView<'__pssz_view, #payload, __PsszFormat>>;));
                methods.push(quote! {
                    fn #method(&self) -> ::core::option::Option<#k::pssz_view::PsszView<'__pssz_view, #payload, __PsszFormat>> {
                        if self.data[0] == #tag { ::core::option::Option::Some(#k::pssz_view::PsszView::new(&self.data[1..])) } else { ::core::option::Option::None }
                    }
                });
            }
        }
        Data::Union(data) => {
            return Err(syn::Error::new_spanned(
                data.union_token,
                "PSSZ views do not support Rust unions",
            ))
        }
    }
    Ok(quote! {
        #(#field_impls)*
        #[allow(non_snake_case)]
        #vis trait #accessor #impl_g #where_c { #(#declarations)* }
        #[allow(non_snake_case)]
        impl #impl_g #accessor #trait_g for #k::pssz_view::PsszView<'__pssz_view, #name #type_g, __PsszFormat> #where_c { #(#methods)* }
    })
}
pub fn derive_view(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    expand_view(input)
        .unwrap_or_else(syn::Error::into_compile_error)
        .into()
}
