//! `#[derive(JsonText)]`: the Json / IJson text formats (`psio::json`).
//!
//! * A struct with named fields is an object in declaration order.
//! * A struct with one unnamed field is transparent, and so is a struct of
//!   one named field marked `#[psio(transparent)]` (or
//!   `#[serde(transparent)]`): its text is its member's.
//! * `#[psio(invariant = "path")]` (a `fn(&Self) -> Result<(), E>` with
//!   `E: Into<Cow<'static, str>>`) is enforced by the writer and the reader: a value that violates
//!   it has no text, and text that reads as one is rejected with its message.
//! * An enum with a variant that holds fields is a variant:
//!   `{"Name": value}` under IJson, `[index, value]` under Json. The name is
//!   the variant identifier unless `#[psio(name = "...")]` renames it (use it
//!   to match the C++ alternative's type name, e.g. "uint32", "string").
//!   The value is the alternative's payload, the one `PsszPack` encodes and
//!   `ToPsszSchema` describes: one unnamed field is that field's text; no
//!   field is the empty record `{}`; named fields are a record (an object,
//!   under `#[serde(rename_all)]` on the variant or `rename_all_fields` on
//!   the enum);
//!   two or more unnamed fields are their tuple (an array). In C++ these
//!   are `std::variant` over `T`, an empty reflected record, a reflected
//!   record and `std::tuple<...>`.
//! * A field-less enum with `#[repr(u8 | ... | i64)]` is written as the
//!   variant's name (the identifier, or `#[psio(name = "...")]`) and read
//!   from the name, the number, the numeric string or `"name:number"`. One
//!   variant may be the catch-all `#[psio(other)] Unknown(<repr>)`: it holds
//!   a value that is no declared variant and is written as its number.
//!   Without it such a number is an error.
//!
//! Integer form, on a field or (as the default for every field) on the
//! struct: `#[json(json_number)]`, `#[json(json_string)]`,
//! `#[json(ijson_number)]`, `#[json(ijson_string)]`.
//!
//! `#[psio(crate = "path")]` on the item names the psio crate (default
//! `::psio`); psio's own types use `#[psio(crate = "crate")]`.

use proc_macro::TokenStream;
use proc_macro2::TokenStream as TokenStream2;
use quote::quote;
use syn::{parse_macro_input, Data, DeriveInput, Fields};
use crate::attrs;

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

fn form_tokens(j: &TokenStream2, form: Option<bool>) -> TokenStream2 {
    match form {
        None => quote!(#j::IntForm::Natural),
        Some(false) => quote!(#j::IntForm::Number),
        Some(true) => quote!(#j::IntForm::String),
    }
}

/// A record of named members as a JSON object: the statements that write
/// it and the expression that reads it. `access` gives the expression of a
/// member's reference, `path` what is constructed (`Self`, `Self::Variant`).
fn record_tokens(
    k: &syn::Path,
    j: &TokenStream2,
    fields: &Fields,
    path: TokenStream2,
    access: &dyn Fn(&syn::Ident) -> TokenStream2,
    forms: attrs::Forms,
    case: Option<attrs::Case>,
) -> syn::Result<(TokenStream2, TokenStream2)> {
    let members = attrs::members(fields)?;
        let mut writes = Vec::new();
        let mut slots = Vec::new();
        let mut arms = Vec::new();
        let mut builds = Vec::new();
        let mut keys = Vec::new();
        let mut index_arms = Vec::new();
        for (index, member) in members.iter().enumerate() {
            let field = member.field;
            let ident = field.ident.as_ref().unwrap();
            let attrs = &member.attrs;
            let access = access(ident);
            // The member's name: its identifier, `#[psio(name)]` /
            // `#[serde(rename)]`, or the identifier under the
            // type's `rename_all` convention.
            let key = attrs::member_name(field, attrs, case);
            if keys.contains(&key) {
                return Err(syn::Error::new_spanned(field, "duplicate member name"));
            }
            let ty = &field.ty;
            let json = form_tokens(&j, attrs.forms.json.or(forms.json));
            let ijson = form_tokens(&j, attrs.forms.ijson.or(forms.ijson));
            let first = index == 0;
            let slot = syn::Ident::new(&format!("__f{index}"), proc_macro2::Span::call_site());
            // A member with bounds: the text is the plain type's, checked
            // against the annotation in both directions.
            let (check_write, check_read) = bound_checks(k, field, attrs)?;
            writes.push(quote! {
                w.key(#key, #first)?;
                let __saved = w.member_form(#json, #ijson);
                { let __member = #access; #check_write }
                <#ty as #j::JsonText>::write_json(#access, w)?;
                w.restore_form(__saved);
            });
            slots.push(quote! { let mut #slot: ::core::option::Option<#ty> = ::core::option::Option::None; });
            keys.push(key.clone());
            let read = quote! {
                {
                    let __saved = r.member_form(#json, #ijson);
                    r.ws();
                    let __at = r.position();
                    let __member = <#ty as #j::JsonText>::read_json(r)?;
                    { let __member = &__member; #check_read }
                    #slot = ::core::option::Option::Some(__member);
                    r.restore_form(__saved);
                }
            };
            arms.push(quote! { #key => #read });
            index_arms.push(quote! { #index => #read });
            builds.push(quote! {
                match #slot {
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
        // A skipped field is its default.
        let built = attrs::construct(path.clone(), fields, &builds)?;
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
        Ok((
            quote! {
                w.raw("{");
                #(#writes)*
                w.raw("}");
            },
            quote! {{
                r.begin(b'{', "json: expected '{'")?;
                #(#slots)*
                if !r.take(b'}') {
                    #members
                }
                r.end_object();
                #built
            }},
        ))
}

/// The statements that check `__member` (a reference) against the field's
/// bounds, for the writer and for the reader (which has the value's
/// position in `__at`). Empty for a field without bounds.
fn bound_checks(
    k: &syn::Path,
    field: &syn::Field,
    attrs: &attrs::Attrs,
) -> syn::Result<(TokenStream2, TokenStream2)> {
    let Some(attrs::Annotated { own, spec }) = attrs::annotated(field, attrs, k)? else {
        return Ok((quote!(), quote!()));
    };
    let check = quote!(#k::pssz::bounds::check::<#own, #spec>(__member));
    Ok((
        quote! {
            if #check.is_err() {
                return ::core::result::Result::Err(w.err("json: member length_bound violated"));
            }
        },
        quote! {
            if #check.is_err() {
                return ::core::result::Result::Err(r.err_at(__at, "json: member length_bound violated"));
            }
        },
    ))
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
    let type_attrs = attrs::item(input, attrs::Derive::JsonText)?;
    let krate = type_attrs.krate();
    let j = quote!(#krate::json);
    let mut generics = input.generics.clone();
    for param in generics.type_params_mut() {
        param.bounds.push(syn::parse_quote!(#j::JsonText));
    }
    let (impl_generics, ty_generics, where_clause) = generics.split_for_impl();
    let shared = &type_attrs;
    let mut json_string = quote!();

    // A struct's members: its fields without the skipped ones.
    let members = match &input.data {
        Data::Struct(data) => attrs::members(&data.fields)?,
        _ => Vec::new(),
    };
    // One unnamed member, the others (if any) skipped: the type is that
    // member, like a transparent one.
    let newtype = matches!(&input.data, Data::Struct(data)
        if matches!(data.fields, Fields::Unnamed(_)) && members.len() == 1);

    let (write, read) = match &input.data {
        Data::Struct(data) if (shared.transparent || newtype) && members.len() == 1 => {
            let member = &members[0].member;
            let ty = &members[0].field.ty;
            let construct = attrs::construct(quote!(Self), &data.fields, &[quote!(__inner)])?;
            json_string = quote!(const JSON_STRING: bool = <#ty as #j::JsonText>::JSON_STRING;);
            let (check_write, check_read) = bound_checks(&krate, members[0].field, &members[0].attrs)?;
            (
                quote! {
                    { let __member = &self.#member; #check_write }
                    <#ty as #j::JsonText>::write_json(&self.#member, w)
                },
                quote! {
                    r.ws();
                    let __at = r.position();
                    let __inner = <#ty as #j::JsonText>::read_json(r)?;
                    { let __member = &__inner; #check_read }
                    ::core::result::Result::Ok(#construct)
                },
            )
        }
        Data::Struct(data) if shared.transparent => {
            return Err(syn::Error::new_spanned(
                &data.fields,
                "a transparent type has exactly one member",
            ))
        }
        Data::Struct(data) => match &data.fields {
            Fields::Named(_) => {
                let (write, read) = record_tokens(
                    &krate,
                    &j,
                    &data.fields,
                    quote!(Self),
                    &|ident| quote!(&self.#ident),
                    type_attrs.forms,
                    type_attrs.rename_all,
                )?;
                (
                    quote! {
                        #write
                        ::core::result::Result::Ok(())
                    },
                    quote! { ::core::result::Result::Ok(#read) },
                )
            }
            _ => {
                return Err(syn::Error::new_spanned(
                    input,
                    "JsonText supports structs with named fields and tuple structs of one member",
                ))
            }
        },
        Data::Enum(data) => {
            if let Some(shape) = crate::c_enum::analyze(input)? {
                let Some(repr) = shape.repr.clone() else {
                    return Err(syn::Error::new_spanned(
                        input,
                        "a field-less JsonText enum needs its integer type: add #[repr(u8)] \
                         (or u16/u32/u64/i8/i16/i32/i64)",
                    ));
                };
                let variants: Vec<_> = shape.declared.iter().map(|v| &v.ident).collect();
                let mut names: Vec<String> = Vec::new();
                for variant in &shape.declared {
                    let label = attrs::variant_name(
                        variant,
                        &attrs::variant(variant)?,
                        type_attrs.rename_all,
                    );
                    // A name must be told apart from a number and from the
                    // "name:number" form.
                    let first = label.bytes().next();
                    if first.is_none_or(|c| c == b'-' || c.is_ascii_digit()) || label.contains(':') {
                        return Err(syn::Error::new_spanned(
                            variant,
                            "an enumerator's JSON name is not empty, does not start with a digit \
                             or '-', and holds no ':'",
                        ));
                    }
                    if names.contains(&label) {
                        return Err(syn::Error::new_spanned(variant, "duplicate enumerator name"));
                    }
                    names.push(label);
                }
                let (preamble, values) = shape.values();
                let signed = shape.signed();
                let (write_other, read_unknown) = match shape.other {
                    Some(other) => {
                        let other = &other.ident;
                        (
                            // One representation per value: the catch-all
                            // never holds a declared enumerator.
                            quote! {
                                Self::#other(__n) => {
                                    #(if *__n == #values {
                                        return ::core::result::Result::Err(w.err(
                                            "json: catch-all enum variant holds a declared enumerator"));
                                    })*
                                    <#repr as #j::JsonText>::write_json(__n, w)
                                }
                            },
                            quote!(::core::result::Result::Ok(Self::#other(__value))),
                        )
                    }
                    None => (
                        quote!(),
                        quote!(::core::result::Result::Err(r.err_at(__at, "json: value is not an enumerator"))),
                    ),
                };
                (
                    // A declared value is its name; the catch-all's value is
                    // its number, by the integer rule in force.
                    quote! {
                        #preamble
                        match self {
                            #(Self::#variants => w.string(#names.as_bytes()),)*
                            #write_other
                        }
                    },
                    quote! {
                        #preamble
                        let __at = r.position();
                        let __value: #repr = #j::read_enum_value::<#repr>(r, #signed, |__name: &str| match __name {
                            #(#names => ::core::option::Option::Some(#values),)*
                            _ => ::core::option::Option::None,
                        })?;
                        #(if __value == #values { return ::core::result::Result::Ok(Self::#variants); })*
                        #read_unknown
                    },
                )
            } else {
                let mut names = Vec::new();
                let mut write_arms = Vec::new();
                let mut read_arms = Vec::new();
                for (index, variant) in data.variants.iter().enumerate() {
                    let ident = &variant.ident;
                    let variant_attrs = attrs::variant(variant)?;
                    let label = attrs::variant_name(variant, &variant_attrs, type_attrs.rename_all);
                    if names.contains(&label) {
                        return Err(syn::Error::new_spanned(variant, "duplicate variant name"));
                    }
                    let index_u32 = index as u32;
                    // The payload, as PsszPack and ToPsszSchema have it: one
                    // unnamed field is that value; no field is the empty
                    // record; named fields are a record; any other number
                    // of unnamed fields is their tuple.
                    let (pattern, write, read) = match &variant.fields {
                        Fields::Unnamed(fields) if fields.unnamed.len() == 1 => {
                            let ty = &fields.unnamed[0].ty;
                            let (check_write, check_read) =
                                bound_checks(&krate, &fields.unnamed[0], &attrs::field(&fields.unnamed[0])?)?;
                            (
                                quote!(Self::#ident(__v)),
                                quote! {
                                    { let __member = __v; #check_write }
                                    <#ty as #j::JsonText>::write_json(__v, w)?;
                                },
                                quote! {{
                                    r.ws();
                                    let __at = r.position();
                                    let __member = <#ty as #j::JsonText>::read_json(r)?;
                                    { let __member = &__member; #check_read }
                                    Self::#ident(__member)
                                }},
                            )
                        }
                        Fields::Unnamed(fields) => {
                            let types: Vec<_> = fields.unnamed.iter().map(|f| &f.ty).collect();
                            let bindings: Vec<_> = (0..types.len())
                                .map(|i| syn::Ident::new(&format!("__m{i}"), proc_macro2::Span::call_site()))
                                .collect();
                            let positions = 0..types.len();
                            let commas = (0..types.len()).map(|i| if i > 0 { quote!(w.raw(",");) } else { quote!() });
                            let mut check_writes = Vec::new();
                            let mut check_reads = Vec::new();
                            for field in &fields.unnamed {
                                let (write, read) = bound_checks(&krate, field, &attrs::field(field)?)?;
                                check_writes.push(write);
                                check_reads.push(read);
                            }
                            (
                                quote!(Self::#ident(#(#bindings),*)),
                                quote! {
                                    let __tuple = w.member_form(#j::IntForm::Natural, #j::IntForm::Natural);
                                    w.raw("[");
                                    #(#commas { let __member = #bindings; #check_writes }
                                      <#types as #j::JsonText>::write_json(#bindings, w)?;)*
                                    w.raw("]");
                                    w.restore_form(__tuple);
                                },
                                quote! {{
                                    let __tuple = r.member_form(#j::IntForm::Natural, #j::IntForm::Natural);
                                    r.begin(b'[', "json: expected '['")?;
                                    let __value = Self::#ident(#({
                                        r.element(#positions)?;
                                        r.ws();
                                        let __at = r.position();
                                        let __member = <#types as #j::JsonText>::read_json(r)?;
                                        { let __member = &__member; #check_reads }
                                        __member
                                    }),*);
                                    r.close_array()?;
                                    r.restore_form(__tuple);
                                    __value
                                }},
                            )
                        }
                        Fields::Named(_) | Fields::Unit => {
                            let binding = |field: &syn::Ident| {
                                let text = field.to_string();
                                syn::Ident::new(
                                    &format!("__m_{}", text.trim_start_matches("r#")),
                                    proc_macro2::Span::call_site(),
                                )
                            };
                            let (write, read) = record_tokens(
                                &krate,
                                &j,
                                &variant.fields,
                                quote!(Self::#ident),
                                &|field| {
                                    let binding = binding(field);
                                    quote!(#binding)
                                },
                                attrs::Forms::default(),
                                variant_attrs.rename_all.or(type_attrs.rename_all_fields),
                            )?;
                            let pattern = match &variant.fields {
                                Fields::Unit => quote!(Self::#ident),
                                fields => {
                                    let members = attrs::members(fields)?;
                                    let fields = members.iter().map(|m| m.field.ident.as_ref().unwrap());
                                    let bindings = members.iter().map(|m| binding(m.field.ident.as_ref().unwrap()));
                                    quote!(Self::#ident { #(#fields: #bindings,)* .. })
                                }
                            };
                            (pattern, write, read)
                        }
                    };
                    write_arms.push(quote! {
                        #pattern => {
                            let __saved = #j::write_variant_open(w, #label, #index_u32)?;
                            #write
                            #j::write_variant_close(w, __saved);
                        }
                    });
                    read_arms.push(quote! {
                        #index => #read,
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

    // The invariant: after the members were written / read (innermost
    // first), before any text or value is returned.
    let (write, read) = match &shared.invariant {
        None => (write, read),
        Some(path) => (
            quote! {
                let __written: ::core::result::Result<(), #j::JsonTextError> = (|| { #write })();
                __written?;
                if let ::core::result::Result::Err(__why) = #path(self) {
                    return ::core::result::Result::Err(w.err(__why));
                }
                ::core::result::Result::Ok(())
            },
            quote! {
                r.ws();
                let __start = r.position();
                let __value: Self = (|| -> ::core::result::Result<Self, #j::JsonTextError> { #read })()?;
                if let ::core::result::Result::Err(__why) = #path(&__value) {
                    return ::core::result::Result::Err(r.err_at(__start, __why));
                }
                ::core::result::Result::Ok(__value)
            },
        ),
    };
    Ok(quote! {
        impl #impl_generics #j::JsonText for #name #ty_generics #where_clause {
            #json_string
            fn write_json(&self, w: &mut #j::Writer) -> ::core::result::Result<(), #j::JsonTextError> {
                #write
            }
            fn read_json(r: &mut #j::Reader<'_>) -> ::core::result::Result<Self, #j::JsonTextError> {
                #read
            }
        }
    })
}
