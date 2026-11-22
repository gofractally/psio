//! `#[derive(JsonText)]`: the Json / IJson text formats (`psio::json`).
//!
//! * A struct with named fields is an object in declaration order.
//! * A struct with one unnamed field is transparent.
//! * An enum whose variants each hold one unnamed field is a variant:
//!   `{"Name": value}` under IJson, `[index, value]` under Json. The name is
//!   the variant identifier unless `#[json(name = "...")]` renames it (use it
//!   to match the C++ alternative's type name, e.g. "uint32", "string").
//! * A field-less enum with `#[repr(u8 | ... | i64)]` is its integer.
//!
//! Integer form, on a field or (as the default for every field) on the
//! struct: `#[json(json_number)]`, `#[json(json_string)]`,
//! `#[json(ijson_number)]`, `#[json(ijson_string)]`.

use proc_macro::TokenStream;
use proc_macro2::TokenStream as TokenStream2;
use quote::quote;
use syn::{parse_macro_input, Data, DeriveInput, Fields, Meta, NestedMeta};

#[derive(Clone, Copy, Default)]
pub(crate) struct Forms {
    pub(crate) json: Option<bool>,  // Some(true) = string
    pub(crate) ijson: Option<bool>,
}

pub(crate) struct Attrs {
    pub(crate) forms: Forms,
    pub(crate) name: Option<String>,
}

/// The `#[json(...)]` attributes of an item, field or variant.
pub(crate) fn parse_attrs(attrs: &[syn::Attribute]) -> syn::Result<Attrs> {
    let mut out = Attrs { forms: Forms::default(), name: None };
    for attr in attrs {
        if !attr.path.is_ident("json") {
            continue;
        }
        let Meta::List(list) = attr.parse_meta()? else {
            return Err(syn::Error::new_spanned(attr, "expected #[json(...)]"));
        };
        for nested in &list.nested {
            match nested {
                NestedMeta::Meta(Meta::Path(path)) if path.is_ident("json_number") => {
                    out.forms.json = Some(false)
                }
                NestedMeta::Meta(Meta::Path(path)) if path.is_ident("json_string") => {
                    out.forms.json = Some(true)
                }
                NestedMeta::Meta(Meta::Path(path)) if path.is_ident("ijson_number") => {
                    out.forms.ijson = Some(false)
                }
                NestedMeta::Meta(Meta::Path(path)) if path.is_ident("ijson_string") => {
                    out.forms.ijson = Some(true)
                }
                NestedMeta::Meta(Meta::NameValue(nv)) if nv.path.is_ident("name") => {
                    let syn::Lit::Str(lit) = &nv.lit else {
                        return Err(syn::Error::new_spanned(&nv.lit, "expected a string"));
                    };
                    out.name = Some(lit.value());
                }
                other => {
                    return Err(syn::Error::new_spanned(
                        other,
                        "unsupported #[json(...)] attribute: expected json_number, json_string, \
                         ijson_number, ijson_string or name = \"...\"",
                    ))
                }
            }
        }
    }
    Ok(out)
}

// ── Member lookup ────────────────────────────────────────────────────────────
//
// A key is matched with a `match` on the member names: rustc compiles it to
// a switch on the key's length followed by compares, which measured as fast
// as a perfect hash built here (benches/json_text_bench.rs) and is what
// records of up to EXPECTED_ORDER_MIN members use, unchanged.
//
// A wider record first compares each key with the member after the last one
// read (writers emit declaration order) and only then falls back to the
// match. That measured ~11% faster on a 200-member record read in declared
// order and the same in reversed order; on 5- to 10-member records the extra
// compare measured ~8% slower, hence the threshold.
const EXPECTED_ORDER_MIN: usize = 64;

/// `match key { "name0" => 0, ..., _ => count }` as a usize expression.
fn lookup_tokens(names: &[String], key: TokenStream2) -> TokenStream2 {
    let count = names.len();
    let indices = 0..count;
    quote! {
        match #key { #(#names => #indices,)* _ => #count }
    }
}

fn form_tokens(form: Option<bool>) -> TokenStream2 {
    match form {
        None => quote!(::psio::json::IntForm::Natural),
        Some(false) => quote!(::psio::json::IntForm::Number),
        Some(true) => quote!(::psio::json::IntForm::String),
    }
}

fn int_repr(input: &DeriveInput) -> Option<syn::Ident> {
    for attr in &input.attrs {
        if !attr.path.is_ident("repr") {
            continue;
        }
        if let Ok(Meta::List(list)) = attr.parse_meta() {
            for nested in &list.nested {
                if let NestedMeta::Meta(Meta::Path(path)) = nested {
                    if let Some(ident) = path.get_ident() {
                        let name = ident.to_string();
                        if matches!(
                            name.as_str(),
                            "u8" | "u16" | "u32" | "u64" | "i8" | "i16" | "i32" | "i64"
                        ) {
                            return Some(ident.clone());
                        }
                    }
                }
            }
        }
    }
    None
}

pub fn derive(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    match expand(&input) {
        Ok(tokens) => tokens.into(),
        Err(err) => err.to_compile_error().into(),
    }
}

fn expand(input: &DeriveInput) -> syn::Result<TokenStream2> {
    let name = &input.ident;
    let type_attrs = parse_attrs(&input.attrs)?;
    let mut generics = input.generics.clone();
    for param in generics.type_params_mut() {
        param.bounds.push(syn::parse_quote!(::psio::json::JsonText));
    }
    let (impl_generics, ty_generics, where_clause) = generics.split_for_impl();
    let j = quote!(::psio::json);

    let (write, read) = match &input.data {
        Data::Struct(data) => match &data.fields {
            Fields::Named(fields) => {
                let mut writes = Vec::new();
                let mut slots = Vec::new();
                let mut arms = Vec::new();
                let mut builds = Vec::new();
                let mut keys = Vec::new();
                let mut index_arms = Vec::new();
                for (index, field) in fields.named.iter().enumerate() {
                    let ident = field.ident.as_ref().unwrap();
                    let key = ident.to_string();
                    let key = key.strip_prefix("r#").unwrap_or(&key).to_string();
                    let ty = &field.ty;
                    let attrs = parse_attrs(&field.attrs)?;
                    if attrs.name.is_some() {
                        return Err(syn::Error::new_spanned(
                            field,
                            "#[json(name)] applies to enum variants",
                        ));
                    }
                    let json = form_tokens(attrs.forms.json.or(type_attrs.forms.json));
                    let ijson = form_tokens(attrs.forms.ijson.or(type_attrs.forms.ijson));
                    let first = index == 0;
                    let slot = syn::Ident::new(&format!("__f{index}"), proc_macro2::Span::call_site());
                    writes.push(quote! {
                        w.key(#key, #first)?;
                        let __saved = w.member_form(#json, #ijson);
                        <#ty as #j::JsonText>::write_json(&self.#ident, w)?;
                        w.restore_form(__saved);
                    });
                    slots.push(quote! { let mut #slot: ::core::option::Option<#ty> = ::core::option::Option::None; });
                    keys.push(key.clone());
                    let read = quote! {
                        {
                            let __saved = r.member_form(#json, #ijson);
                            #slot = ::core::option::Option::Some(<#ty as #j::JsonText>::read_json(r)?);
                            r.restore_form(__saved);
                        }
                    };
                    arms.push(quote! { #key => #read });
                    index_arms.push(quote! { #index => #read });
                    builds.push(quote! {
                        #ident: match #slot {
                            ::core::option::Option::Some(v) => v,
                            ::core::option::Option::None => match <#ty as #j::JsonText>::json_missing() {
                                ::core::option::Option::Some(v) => v,
                                ::core::option::Option::None => {
                                    return ::core::result::Result::Err(r.err("json: missing required member"))
                                }
                            },
                        }
                    });
                }
                let count = keys.len();
                let members = if count > EXPECTED_ORDER_MIN {
                    let lookup = lookup_tokens(&keys, quote!(__key));
                    quote! {
                        const __NAMES: [&str; #count] = [#(#keys),*];
                        // The member after the last one read: writers emit
                        // declaration order, so it is tried first, with one
                        // compare, before the match.
                        let mut __expected: usize = 0;
                        loop {
                            // A repeated key is read again: the last one wins.
                            let __key = r.key()?;
                            let __key: &str = &__key;
                            let __index: usize =
                                if __expected < #count && __NAMES[__expected] == __key {
                                    __expected
                                } else {
                                    #lookup
                                };
                            match __index {
                                #(#index_arms)*
                                _ => r.skip_unknown()?,
                            }
                            if __index < #count {
                                __expected = __index + 1;
                            }
                            if r.object_next()? {
                                break;
                            }
                        }
                    }
                } else {
                    quote! {
                        loop {
                            // A repeated key is read again: the last one wins.
                            let __key = r.key()?;
                            match &*__key {
                                #(#arms)*
                                _ => r.skip_unknown()?,
                            }
                            if r.object_next()? {
                                break;
                            }
                        }
                    }
                };
                (
                    quote! {
                        w.raw("{");
                        #(#writes)*
                        w.raw("}");
                        ::core::result::Result::Ok(())
                    },
                    quote! {
                        r.begin(b'{', "json: expected '{'")?;
                        #(#slots)*
                        if !r.take(b'}') {
                            #members
                        }
                        r.end_object();
                        ::core::result::Result::Ok(Self { #(#builds),* })
                    },
                )
            }
            Fields::Unnamed(fields) if fields.unnamed.len() == 1 => {
                let ty = &fields.unnamed[0].ty;
                (
                    quote! { <#ty as #j::JsonText>::write_json(&self.0, w) },
                    quote! { ::core::result::Result::Ok(Self(<#ty as #j::JsonText>::read_json(r)?)) },
                )
            }
            _ => {
                return Err(syn::Error::new_spanned(
                    input,
                    "JsonText supports structs with named fields and single-field tuple structs",
                ))
            }
        },
        Data::Enum(data) => {
            let fieldless = data.variants.iter().all(|v| matches!(v.fields, Fields::Unit));
            if fieldless {
                let Some(repr) = int_repr(input) else {
                    return Err(syn::Error::new_spanned(
                        input,
                        "a field-less JsonText enum is written as its integer: add #[repr(u8)] \
                         (or u16/u32/u64/i8/i16/i32/i64)",
                    ));
                };
                let variants: Vec<_> = data.variants.iter().map(|v| &v.ident).collect();
                (
                    quote! {
                        let __value: #repr = match self { #(Self::#variants => Self::#variants as #repr),* };
                        <#repr as #j::JsonText>::write_json(&__value, w)
                    },
                    quote! {
                        let __at = r.position();
                        let __value = <#repr as #j::JsonText>::read_json(r)?;
                        #(if __value == Self::#variants as #repr { return ::core::result::Result::Ok(Self::#variants); })*
                        ::core::result::Result::Err(r.err_at(__at, "json: value is not an enumerator"))
                    },
                )
            } else {
                let mut names = Vec::new();
                let mut write_arms = Vec::new();
                let mut read_arms = Vec::new();
                for (index, variant) in data.variants.iter().enumerate() {
                    let ident = &variant.ident;
                    let Fields::Unnamed(fields) = &variant.fields else {
                        return Err(syn::Error::new_spanned(
                            variant,
                            "a JsonText variant holds exactly one unnamed field",
                        ));
                    };
                    if fields.unnamed.len() != 1 {
                        return Err(syn::Error::new_spanned(
                            variant,
                            "a JsonText variant holds exactly one unnamed field",
                        ));
                    }
                    let ty = &fields.unnamed[0].ty;
                    let label = parse_attrs(&variant.attrs)?.name.unwrap_or_else(|| ident.to_string());
                    if names.contains(&label) {
                        return Err(syn::Error::new_spanned(variant, "duplicate variant name"));
                    }
                    let index_u32 = index as u32;
                    write_arms.push(quote! {
                        Self::#ident(__v) => {
                            let __saved = #j::write_variant_open(w, #label, #index_u32)?;
                            <#ty as #j::JsonText>::write_json(__v, w)?;
                            #j::write_variant_close(w, __saved);
                        }
                    });
                    read_arms.push(quote! {
                        #index => Self::#ident(<#ty as #j::JsonText>::read_json(r)?),
                    });
                    names.push(label);
                }
                let variant_count = names.len();
                let variant_lookup = lookup_tokens(&names, quote!(__key));
                (
                    quote! {
                        match self { #(#write_arms)* }
                        ::core::result::Result::Ok(())
                    },
                    quote! {
                        let (__index, __saved) = #j::read_variant_open_by(
                            r, #variant_count, |__key: &str| -> usize { #variant_lookup })?;
                        let __value = match __index {
                            #(#read_arms)*
                            _ => unreachable!("read_variant_open returns a valid index"),
                        };
                        #j::read_variant_close(r, __saved)?;
                        ::core::result::Result::Ok(__value)
                    },
                )
            }
        }
        Data::Union(_) => {
            return Err(syn::Error::new_spanned(input, "JsonText does not support unions"))
        }
    };

    Ok(quote! {
        impl #impl_generics #j::JsonText for #name #ty_generics #where_clause {
            fn write_json(&self, w: &mut #j::Writer) -> ::core::result::Result<(), #j::JsonTextError> {
                #write
            }
            fn read_json(r: &mut #j::Reader<'_>) -> ::core::result::Result<Self, #j::JsonTextError> {
                #read
            }
        }
    })
}
