//! The pSSZ derives: `PsszPack`, `PsszUnpack`, `PsszView`, `ToPsszSchema`.
use proc_macro::TokenStream;
use proc_macro2::TokenStream as Tokens;
use quote::{format_ident, quote};
use syn::{
    parse_macro_input, parse_quote, Data, DeriveInput, Fields, Member, Meta, NestedMeta, Type,
};
use crate::attrs::{self, Case, Derive, Forms};

struct Options {
    krate: syn::Path,
    dwnc: bool,
    transparent: bool,
    name: Option<String>,
    /// Explicit maxDynamicData cap: `#[pssz(max_dynamic_data = N)]`.
    cap: Option<u64>,
    /// `#[psio(variant)]`: encode a field-less enum as a variant of unit
    /// alternatives instead of as its underlying integer.
    variant: bool,
    /// `#[psio(custom)]` / `#[psio(custom = "Id")]`: the type has a custom
    /// text form (`TextForm`). Its schema is `Custom { wire type, id }`; the
    /// id defaults to the schema name. The bytes are unaffected.
    custom: Option<Option<String>>,
    /// `#[psio(invariant = "path")]`: the type's invariant, a
    /// `fn(&Self) -> Result<(), E>` with `E: Into<Cow<'static, str>>`
    /// (docs/pssz-spec.md 18). Every
    /// checking reader and every writer enforces it; the schema node is
    /// `Custom { wire type, id }`.
    invariant: Option<syn::Path>,
    /// The JSON integer forms of the type's members.
    forms: Forms,
    /// The convention of the member names (a struct) or the variant names
    /// (an enum), and that of the members of struct variants.
    rename_all: Option<Case>,
    rename_all_fields: Option<Case>,
}
use crate::c_enum::{self, CEnum};

/// A field-less enum encoded as its underlying integer (C++ plain `enum`).
fn c_like<'a>(input: &'a DeriveInput, opts: &Options) -> syn::Result<Option<CEnum<'a>>> {
    let Some(shape) = c_enum::analyze(input)? else { return Ok(None) };
    if opts.variant {
        if let Some(other) = shape.other {
            return Err(syn::Error::new_spanned(other, "#[psio(other)] cannot be combined with #[psio(variant)]"));
        }
        return Ok(None);
    }
    if shape.repr.is_none() {
        return Err(syn::Error::new_spanned(&input.ident,
            "a field-less PSSZ enum encodes as its underlying integer: add #[repr(u8)] (or u16/u32/u64/i8/i16/i32/i64), or #[psio(variant)] to encode it as a variant of unit alternatives"));
    }
    Ok(Some(shape))
}
/// `#[repr(packed)]` / `#[repr(packed(N))]`: a field may be unaligned, so it
/// cannot be borrowed. The generated code copies each field instead, which
/// makes every field of a packed record `Copy`.
fn packed(input: &DeriveInput) -> bool {
    input.attrs.iter().any(|attr| {
        attr.path.is_ident("repr")
            && matches!(attr.parse_meta(), Ok(Meta::List(list)) if list.nested.iter().any(|item| match item {
                NestedMeta::Meta(Meta::Path(path)) => path.is_ident("packed"),
                NestedMeta::Meta(Meta::List(inner)) => inner.path.is_ident("packed"),
                _ => false,
            }))
    })
}
/// A borrow of field `member` of `self`.
fn field_ref(packed: bool, member: &Member) -> Tokens {
    if packed {
        quote!(&{ self.#member })
    } else {
        quote!(&self.#member)
    }
}
fn options(input: &DeriveInput, derive: Derive) -> syn::Result<Options> {
    for param in &input.generics.params {
        let reserved = match param {
            syn::GenericParam::Type(t) => t.ident == "__PsszFormat",
            syn::GenericParam::Const(c) => c.ident == "__PsszFormat",
            syn::GenericParam::Lifetime(l) => l.lifetime.ident == "__pssz_view",
        };
        if reserved { return Err(syn::Error::new_spanned(param, "generic name is reserved by PSSZ derives")); }
    }
    let item = attrs::item(input, derive)?;
    let out = Options {
        krate: item.krate(),
        dwnc: item.definition_will_not_change,
        transparent: item.transparent,
        name: item.name,
        cap: item.max_dynamic_data,
        variant: item.variant,
        custom: item.custom,
        invariant: item.invariant,
        forms: item.forms,
        rename_all: item.rename_all,
        rename_all_fields: item.rename_all_fields,
    };
    if out.cap.is_some() && (out.transparent || !matches!(input.data, Data::Struct(_))) {
        return Err(syn::Error::new_spanned(
            input,
            "max_dynamic_data applies to non-transparent PSSZ records only",
        ));
    }
    if out.variant && !matches!(input.data, Data::Enum(_)) {
        return Err(syn::Error::new_spanned(input, "#[psio(variant)] applies to enums only"));
    }
    if out.transparent && out.dwnc {
        return Err(syn::Error::new_spanned(
            input,
            "transparent and definition_will_not_change cannot be combined",
        ));
    }
    Ok(out)
}

/// The members of a record: its fields without the skipped ones
/// (`#[psio(skip)]`), which are in no format.
///
/// A member's type here is its wire type: for a member with bounds
/// (`#[psio(max = N, element(...))]`) the type its annotation describes,
/// which has the bound, the widths, the validator and the reader. Values go
/// through `encode_value` / `decode_value`.
fn field_info<'a>(k: &syn::Path, fields: &'a Fields) -> syn::Result<Vec<(Member, &'a Type)>> {
    attrs::members(fields)?
        .into_iter()
        .map(|m| {
            Ok(match attrs::annotated(m.field, &m.attrs, k)? {
                // The derive runs once: the described type lives as long.
                Some(annotated) => (m.member, &*Box::leak(Box::new(annotated.described(k)))),
                None => (m.member, &m.field.ty),
            })
        })
        .collect()
}

/// The annotation of each member, in member order.
fn field_bounds(k: &syn::Path, fields: &Fields) -> syn::Result<Vec<Option<attrs::Annotated>>> {
    attrs::members(fields)?.into_iter().map(|m| attrs::annotated(m.field, &m.attrs, k)).collect()
}

/// What the encoder is given for a member: the reference itself, or for a
/// member with bounds the reference under its annotation.
fn encode_value(k: &syn::Path, bound: &Option<attrs::Annotated>, reference: Tokens) -> Tokens {
    match bound {
        Some(attrs::Annotated { own, spec }) => {
            quote!(&#k::pssz::bounds::Described::<#own, #spec>::new(#reference))
        }
        None => reference,
    }
}

/// A decoded member as the field's own type.
fn decode_value(k: &syn::Path, bound: &Option<attrs::Annotated>, value: Tokens) -> Tokens {
    match bound {
        Some(attrs::Annotated { own, spec }) => {
            quote!(<#own as #k::pssz::bounds::Bounds<#spec>>::from_as(#value))
        }
        None => value,
    }
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
        // A skipped field is not encoded, so its type needs no codec.
        .filter(|field| !attrs::field(field).is_ok_and(|a| a.skip))
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

/// Width / cap of the record being generated: the struct itself, or for an
/// enum alternative with several fields the equivalent positional tuple.
struct Frame { width: Tokens, cap: Tokens }
fn self_frame(k: &syn::Path) -> Frame {
    Frame {
        width: quote!(<Self as #k::pssz::PsszPack<__PsszFormat>>::OFFSET_WIDTH),
        cap: quote!(<Self as #k::pssz::PsszPack<__PsszFormat>>::MAX_DYNAMIC_DATA),
    }
}
fn tuple_frame(k: &syn::Path, types: &[&Type]) -> Frame {
    Frame {
        width: quote!(<(#(#types,)*) as #k::pssz::PsszPack<__PsszFormat>>::OFFSET_WIDTH),
        cap: quote!(::core::option::Option::None),
    }
}

fn record_encode(k: &syn::Path, types: &[&Type], values: &[Tokens], dwnc: bool, frame: &Frame) -> Tokens {
    let indices: Vec<_> = (0..types.len()).collect();
    let (width, cap) = (&frame.width, &frame.cap);
    quote! {
        let __omitted: &[bool] = &[
            #(<#types as #k::pssz::PsszPack<__PsszFormat>>::IS_OPTIONAL && #k::pssz::PsszPack::<__PsszFormat>::pssz_is_none(#values)),*
        ];
        let __keep = #k::pssz_checked::record_kept_fields(__omitted, #dwnc);
        let _ = __keep;
        #k::pssz_checked::finish_record::<__PsszFormat>(#width, #cap, &[
            #(#k::pssz_checked::field_if_kept::<__PsszFormat, _>(#values, #indices < __keep)?),*
        ], #dwnc, out)
    }
}

fn record_decode(k: &syn::Path, fields: &Fields, construct: Tokens, dwnc: bool, frame: &Frame) -> syn::Result<Tokens> {
    let info = field_info(k, fields)?;
    let types: Vec<_> = info.iter().map(|(_, ty)| *ty).collect();
    let values: Vec<_> = (0..info.len())
        .map(|i| format_ident!("__pssz_field_{i}"))
        .collect();
    let bounds = field_bounds(k, fields)?;
    let initializer = attrs::construct(
        construct,
        fields,
        &values.iter().zip(&bounds).map(|(v, bound)| decode_value(k, bound, quote!(#v))).collect::<Vec<_>>(),
    )?;
    let (width, cap) = (&frame.width, &frame.cap);
    Ok(quote! {
        #k::pssz_checked::enter_record::<__PsszFormat>(#cap, bytes)?;
        let __spans = #k::pssz_checked::record_spans_n(#width, [
            #(#k::pssz_checked::Layout::of::<__PsszFormat, #types>()),*
        ], bytes, #dwnc)?;
        #[allow(unused_mut)]
        let mut __spans = __spans.into_iter();
        #(let #values = #k::pssz_checked::decode_field::<__PsszFormat, #types>(bytes,
            __spans.next().flatten())?;)*
        ::core::result::Result::Ok(#initializer)
    })
}

/// How a member is read from bytes the caller vouches for: by the trusted
/// reader of its own type where its annotation leaves the bytes alone (the
/// bounds are not checked), else through the described type.
fn trusted_value(k: &syn::Path, ty: &Type, bound: &Option<attrs::Annotated>, bytes: Tokens) -> Tokens {
    match bound {
        Some(attrs::Annotated { own, spec }) => quote! {
            if <#own as #k::pssz::bounds::Bounds<#spec>>::SAME_BYTES {
                <#own as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_unpack_trusted(#bytes)
            } else {
                <#own as #k::pssz::bounds::Bounds<#spec>>::from_as(
                    <#ty as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_unpack_trusted(#bytes))
            }
        },
        None => quote!(<#ty as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_unpack_trusted(#bytes)),
    }
}

/// The trusted reader of a record: no depth, cap, bound or invariant
/// check. A fixed-layout record is read at constant offsets.
fn record_decode_trusted(k: &syn::Path, fields: &Fields, construct: Tokens, dwnc: bool, frame: &Frame) -> syn::Result<Tokens> {
    let info = field_info(k, fields)?;
    let bounds = field_bounds(k, fields)?;
    let types: Vec<_> = info.iter().map(|(_, ty)| *ty).collect();
    let values: Vec<_> = (0..info.len()).map(|i| format_ident!("__pssz_field_{i}")).collect();
    let initializer =
        attrs::construct(construct, fields, &values.iter().map(|v| quote!(#v)).collect::<Vec<_>>())?;
    let fixed_reads: Vec<_> = types
        .iter()
        .zip(&bounds)
        .map(|(ty, bound)| trusted_value(k, ty, bound, quote!(&bytes[__at..__at + __size])))
        .collect();
    let span_reads: Vec<_> =
        types.iter().zip(&bounds).map(|(ty, bound)| trusted_value(k, ty, bound, quote!(&bytes[__span]))).collect();
    let missing: Vec<_> = types
        .iter()
        .zip(&bounds)
        .map(|(ty, bound)| {
            let value = decode_value(k, bound, quote!(__missing));
            quote! {
                match <#ty as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_missing_optional() {
                    ::core::result::Result::Ok(__missing) => #value,
                    ::core::result::Result::Err(error) => #k::pssz::trusted_bytes_invalid(error),
                }
            }
        })
        .collect();
    let width = &frame.width;
    Ok(quote! {
        if <Self as #k::pssz::PsszPack<__PsszFormat>>::IS_FIXED_SIZE {
            #[allow(unused_mut)]
            let mut __at = 0usize;
            #(let #values = {
                let __size = <#types as #k::pssz::PsszPack<__PsszFormat>>::FIXED_SIZE;
                let __value = #fixed_reads;
                __at += __size;
                __value
            };)*
            let _ = __at;
            return #initializer;
        }
        let __spans = match #k::pssz_checked::record_spans_n(#width, [
            #(#k::pssz_checked::Layout::of::<__PsszFormat, #types>()),*
        ], bytes, #dwnc) {
            ::core::result::Result::Ok(spans) => spans,
            ::core::result::Result::Err(error) => #k::pssz::trusted_bytes_invalid(error),
        };
        #[allow(unused_mut)]
        let mut __spans = __spans.into_iter();
        #(let #values = match __spans.next().flatten() {
            ::core::option::Option::Some(__span) => #span_reads,
            ::core::option::Option::None => #missing,
        };)*
        #initializer
    })
}

fn record_validate(k: &syn::Path, fields: &Fields, dwnc: bool, frame: &Frame) -> syn::Result<Tokens> {
    let info = field_info(k, fields)?;
    let types: Vec<_> = info.iter().map(|(_, ty)| *ty).collect();
    let (width, cap) = (&frame.width, &frame.cap);
    Ok(quote! {
        #k::pssz_checked::enter_record::<__PsszFormat>(#cap, bytes)?;
        let __spans = #k::pssz_checked::record_spans_n(#width, [
            #(#k::pssz_checked::Layout::of::<__PsszFormat, #types>()),*
        ], bytes, #dwnc)?;
        #[allow(unused_mut)]
        let mut __spans = __spans.into_iter();
        #(if let ::core::option::Option::Some(span) = __spans.next().flatten() {
            <#types as #k::pssz::PsszValidate<__PsszFormat>>::pssz_validate(&bytes[span])?;
        })*
        ::core::result::Result::Ok(())
    })
}

/// Associated consts of the generated `PsszPack` impl.
struct Consts { fixed: Tokens, size: Tokens, min: Tokens, extra: Tokens }

fn expand(input: DeriveInput, unpack: bool) -> syn::Result<Tokens> {
    let opts = options(&input, Derive::Pssz)?;
    let k = &opts.krate;
    let name = &input.ident;
    let pack = quote!(#k::pssz::PsszPack<__PsszFormat>);
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
    let c_enum = c_like(&input, &opts)?;
    // Wire-transparent items (a transparent newtype, a C-like enum) forward to
    // the inner codec, which owns the single depth node.
    let forwards = opts.transparent || c_enum.is_some();
    let validation_body;
    let mut missing_optional = quote!();
    // The reader of bytes the caller vouches for, where the derive has one.
    let mut trusted_body: Option<Tokens> = None;
    let (body, consts) = match &input.data {
        Data::Struct(data) => {
            let info = field_info(k, &data.fields)?;
            let types: Vec<_> = info.iter().map(|(_, ty)| *ty).collect();
            let members: Vec<_> = info.iter().map(|(m, _)| m).collect();
            if opts.transparent {
                if info.len() != 1 {
                    return Err(syn::Error::new_spanned(
                        &data.fields,
                        "transparent PSSZ structs require exactly one member",
                    ));
                }
                let ty = types[0];
                let member = members[0];
                let bounds = field_bounds(k, &data.fields)?;
                let member_ref = encode_value(k, &bounds[0], field_ref(packed(&input), member));
                let construct = |value: Tokens| attrs::construct(quote!(Self), &data.fields, &[decode_value(k, &bounds[0], value)]);
                let decoded = construct(quote!(<#ty as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_unpack(bytes)?))?;
                let missing = construct(quote!(<#ty as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_missing_optional()?))?;
                missing_optional = quote! { fn pssz_missing_optional() -> #k::pssz::PsszResult<Self> { ::core::result::Result::Ok(#missing) } };
                if unpack {
                    let inner = trusted_value(k, ty, &bounds[0], quote!(bytes));
                    trusted_body = Some(attrs::construct(quote!(Self), &data.fields, &[inner])?);
                }
                validation_body = quote! { <#ty as #k::pssz::PsszValidate<__PsszFormat>>::pssz_validate(bytes) };
                (
                    if unpack {
                        quote! { ::core::result::Result::Ok(#decoded) }
                    } else {
                        quote! { #k::pssz::PsszPack::<__PsszFormat>::pssz_pack_checked(#member_ref, out) }
                    },
                    Consts {
                        fixed: quote!(<#ty as #pack>::IS_FIXED_SIZE),
                        size: quote!(<#ty as #pack>::FIXED_SIZE),
                        min: quote!(<#ty as #pack>::MIN_ENCODED_SIZE),
                        extra: quote! {
                            const IS_OPTIONAL: bool = <#ty as #pack>::IS_OPTIONAL;
                            const OPTIONAL_SELECTOR: bool = <#ty as #pack>::OPTIONAL_SELECTOR;
                            fn pssz_is_none(&self) -> bool { #k::pssz::PsszPack::<__PsszFormat>::pssz_is_none(#member_ref) }
                            fn pssz_none_bytes() -> ::core::option::Option<&'static [u8]> { <#ty as #pack>::pssz_none_bytes() }
                            const MAX_ENCODED_SIZE: ::core::option::Option<usize> = <#ty as #pack>::MAX_ENCODED_SIZE;
                            const MAX_DYNAMIC_DATA: ::core::option::Option<usize> = <#ty as #pack>::MAX_DYNAMIC_DATA;
                            const AUTO_BOUND: ::core::option::Option<usize> = <#ty as #pack>::AUTO_BOUND;
                            const OFFSET_WIDTH: usize = <#ty as #pack>::OFFSET_WIDTH;
                            const SHAPE: #k::pssz::Shape = <#ty as #pack>::SHAPE;
                        },
                    },
                )
            } else {
                let dwnc = opts.dwnc;
                let frame = self_frame(k);
                let packed = packed(&input);
                let bounds = field_bounds(k, &data.fields)?;
                let values = members
                    .iter()
                    .zip(&bounds)
                    .map(|(m, bound)| encode_value(k, bound, field_ref(packed, m)))
                    .collect::<Vec<_>>();
                let cap = match opts.cap {
                    Some(cap) => { let cap = cap as usize; quote!(::core::option::Option::Some(#cap)) }
                    None => quote!(::core::option::Option::None),
                };
                validation_body = record_validate(k, &data.fields, dwnc, &frame)?;
                if unpack {
                    trusted_body = Some(record_decode_trusted(k, &data.fields, quote!(Self), dwnc, &frame)?);
                }
                (
                    if unpack {
                        record_decode(k, &data.fields, quote!(Self), dwnc, &frame)?
                    } else {
                        record_encode(k, &types, &values, dwnc, &frame)
                    },
                    Consts {
                        fixed: quote!(#dwnc #(&& <#types as #pack>::IS_FIXED_SIZE)*),
                        size: quote!(0usize #(.saturating_add(<#types as #pack>::FIXED_SIZE))*),
                        // An extensible record always has its header; a DWNC
                        // record is as small as its fixed region.
                        min: if dwnc {
                            quote!(#k::pssz_checked::fixed_region(1, &[#(#k::pssz_checked::Layout::of::<__PsszFormat, #types>()),*]))
                        } else {
                            quote!(1)
                        },
                        extra: quote! {
                            const MAX_DYNAMIC_DATA: ::core::option::Option<usize> = #cap;
                            const AUTO_BOUND: ::core::option::Option<usize> = match <Self as #pack>::MAX_DYNAMIC_DATA {
                                ::core::option::Option::Some(cap) => ::core::option::Option::Some(cap),
                                ::core::option::Option::None => #k::pssz_checked::record_bound(&[
                                    #((<#types as #pack>::AUTO_BOUND, <#types as #pack>::IS_FIXED_SIZE)),*
                                ], #dwnc),
                            };
                            const MAX_ENCODED_SIZE: ::core::option::Option<usize> = <Self as #pack>::AUTO_BOUND;
                            const SHAPE: #k::pssz::Shape = #k::pssz::Shape::record(
                                false, #dwnc, <Self as #pack>::OFFSET_WIDTH, <Self as #pack>::IS_FIXED_SIZE,
                                &[#(<#types as #pack>::SHAPE),*]);
                        },
                    },
                )
            }
        }
        Data::Enum(_) if c_enum.is_some() => {
            // C++ plain enum: the underlying integer, fixed size. With a
            // #[psio(other)] catch-all every value of the integer decodes;
            // without one only the declared enumerators do.
            let shape = c_enum.as_ref().unwrap();
            let repr = shape.repr.as_ref().unwrap();
            if opts.transparent || opts.dwnc {
                return Err(syn::Error::new_spanned(input, "PSSZ enums cannot be transparent or DWNC"));
            }
            let variants: Vec<_> = shape.declared.iter().map(|v| &v.ident).collect();
            let (preamble, values) = shape.values();
            let shape_name = format!("enum:{}", name);
            let (unknown_ok, unknown_value, pack_other) = match shape.other {
                Some(other) => {
                    let other = &other.ident;
                    (
                        quote!(::core::result::Result::Ok(())),
                        quote!(::core::result::Result::Ok(Self::#other(__value))),
                        // One representation per value: the catch-all never
                        // holds a declared enumerator.
                        quote! {
                            Self::#other(__n) => {
                                #(if *__n == #values {
                                    return ::core::result::Result::Err(#k::pssz::PsszError::new(
                                        "pssz: catch-all enum variant holds a declared enumerator"));
                                })*
                                *__n
                            }
                        },
                    )
                }
                None => (
                    quote!(::core::result::Result::Err(#k::pssz::PsszError::new("pssz: value is not an enumerator"))),
                    quote!(::core::result::Result::Err(#k::pssz::PsszError::new("pssz: value is not an enumerator"))),
                    quote!(),
                ),
            };
            validation_body = quote! {
                #preamble
                let __value = <#repr as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_unpack(bytes)?;
                #(if __value == #values { return ::core::result::Result::Ok(()); })*
                #unknown_ok
            };
            (
                if unpack {
                    quote! {
                        #preamble
                        let __value = <#repr as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_unpack(bytes)?;
                        #(if __value == #values { return ::core::result::Result::Ok(Self::#variants); })*
                        #unknown_value
                    }
                } else {
                    quote! {
                        #preamble
                        let __value: #repr = match self {
                            #(Self::#variants => #values,)*
                            #pack_other
                        };
                        <#repr as #pack>::pssz_pack_checked(&__value, out)
                    }
                },
                Consts {
                    fixed: quote!(true),
                    size: quote!(::core::mem::size_of::<#repr>()),
                    min: quote!(::core::mem::size_of::<#repr>()),
                    extra: quote! {
                        const MAX_ENCODED_SIZE: ::core::option::Option<usize> = ::core::option::Option::Some(::core::mem::size_of::<#repr>());
                        const AUTO_BOUND: ::core::option::Option<usize> = ::core::option::Option::Some(::core::mem::size_of::<#repr>());
                        const SHAPE: #k::pssz::Shape = #k::pssz::Shape::leaf(#shape_name, ::core::mem::size_of::<#repr>(), true);
                    },
                },
            )
        }
        Data::Enum(data) => {
            // C++ std::variant: one index byte (declaration order), then the
            // alternative. A unit alternative is a zero-field fixed-layout
            // record (no bytes); several fields form a positional tuple.
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
            let mut validate_arms = Vec::new();
            let mut shapes = Vec::new();
            let mut bounds = Vec::new();
            for (index, variant) in data.variants.iter().enumerate() {
                if variant.discriminant.is_some() {
                    return Err(syn::Error::new_spanned(variant, "PSSZ selectors use declaration order; explicit discriminants are unsupported"));
                }
                let vname = &variant.ident;
                let info = field_info(k, &variant.fields)?;
                let types: Vec<_> = info.iter().map(|(_, ty)| *ty).collect();
                let bindings: Vec<_> = (0..info.len())
                    .map(|i| format_ident!("__pssz_value_{i}"))
                    .collect();
                let tag = index as u8;
                let single = matches!(&variant.fields, Fields::Unnamed(_)) && info.len() == 1;
                let variant_bounds = field_bounds(k, &variant.fields)?;
                let frame = tuple_frame(k, &types);
                // What the alternative reserves: nothing, its one
                // payload, or the tuple of its fields.
                bounds.push(match &variant.fields {
                    Fields::Unit => quote!(::core::option::Option::Some(0usize)),
                    _ if single => { let ty = types[0]; quote!(<#ty as #pack>::AUTO_BOUND) }
                    _ => quote!(<(#(#types,)*) as #pack>::AUTO_BOUND),
                });
                shapes.push(match &variant.fields {
                    Fields::Unit => quote!(#k::pssz::Shape::record(false, true, #k::pssz::offset_width::<__PsszFormat>(::core::option::Option::Some(0)), true, &[])),
                    _ if single => { let ty = types[0]; quote!(<#ty as #pack>::SHAPE) }
                    _ => quote!(<(#(#types,)*) as #pack>::SHAPE),
                });
                validate_arms.push(match &variant.fields {
                    Fields::Unit => quote! { #tag => { let _payload = #k::pssz_checked::DecodeDepth::enter()?; if bytes.is_empty() { ::core::result::Result::Ok(()) } else { ::core::result::Result::Err(#k::pssz::PsszError::new("pssz: zero-size fixed payload must be empty")) } } },
                    _ if single => { let ty = types[0]; quote! { #tag => { <#ty as #k::pssz::PsszValidate<__PsszFormat>>::pssz_validate(bytes) } } }
                    _ => { let body = record_validate(k, &variant.fields, false, &frame)?; quote! { #tag => { let _payload = #k::pssz_checked::DecodeDepth::enter()?; #body } } }
                });
                if unpack {
                    let decode = match &variant.fields {
                        Fields::Unit => {
                            quote! { let _payload_depth = #k::pssz_checked::DecodeDepth::enter()?; if !bytes.is_empty() { return ::core::result::Result::Err(#k::pssz::PsszError::new("pssz: zero-size fixed payload must be empty")); } ::core::result::Result::Ok(Self::#vname) }
                        }
                        _ if single => {
                            let ty = types[0];
                            let value = decode_value(k, &variant_bounds[0], quote!(<#ty as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_unpack(bytes)?));
                            quote! { ::core::result::Result::Ok(Self::#vname(#value)) }
                        }
                        _ => {
                            let body = record_decode(k, &variant.fields, quote!(Self::#vname), false, &frame)?;
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
                        _ if single => {
                            let b = &bindings[0];
                            let value = encode_value(k, &variant_bounds[0], quote!(#b));
                            quote!(#k::pssz::PsszPack::<__PsszFormat>::pssz_pack_checked(#value, out))
                        }
                        _ => {
                            let body = record_encode(
                                k,
                                &types,
                                &bindings
                                    .iter()
                                    .zip(&variant_bounds)
                                    .map(|(b, bound)| encode_value(k, bound, quote!(#b)))
                                    .collect::<Vec<_>>(),
                                false,
                                &frame,
                            );
                            quote! { let _payload_depth = #k::pssz_checked::EncodeDepth::enter()?; #body }
                        }
                    };
                    arms.push(quote!(#pattern => { out.push(#tag); #encode }));
                }
            }
            validation_body = quote! {
                let (&selector, bytes) = bytes.split_first().ok_or(#k::pssz::PsszError::new("pssz: variant span out of bounds"))?;
                match selector { #(#validate_arms,)* _ => ::core::result::Result::Err(#k::pssz::PsszError::new("pssz: variant selector out of range")) }
            };
            let body = if unpack {
                quote! {
                    let (&selector, bytes) = bytes.split_first().ok_or(#k::pssz::PsszError::new("pssz: variant span out of bounds"))?;
                    match selector { #(#arms,)* _ => ::core::result::Result::Err(#k::pssz::PsszError::new("pssz: variant selector out of range")) }
                }
            } else {
                quote! { match self { #(#arms),* } }
            };
            (
                body,
                Consts {
                    fixed: quote!(false),
                    size: quote!(0),
                    min: quote!(1),
                    extra: quote! {
                        // One selector byte, then the widest alternative
                        // (C++ max_encoded_size of a std::variant).
                        const AUTO_BOUND: ::core::option::Option<usize> =
                            #k::pssz::variant_bound(&[#(#bounds),*]);
                        const MAX_ENCODED_SIZE: ::core::option::Option<usize> = <Self as #pack>::AUTO_BOUND;
                        const SHAPE: #k::pssz::Shape = #k::pssz::Shape::node(
                            #k::pssz::ShapeKind::Variant, 0, <Self as #pack>::OFFSET_WIDTH, false, &[#(#shapes),*]);
                    },
                },
            )
        }
        Data::Union(data) => {
            return Err(syn::Error::new_spanned(
                data.union_token,
                "PSSZ does not support Rust unions",
            ))
        }
    };
    let depth_guard = if forwards {
        quote!()
    } else {
        quote!(let _depth = #k::pssz_checked::DecodeDepth::enter()?;)
    };
    let encode_depth_guard = if forwards {
        quote!()
    } else {
        quote!(let _depth = #k::pssz_checked::EncodeDepth::enter()?;)
    };
    let Consts { fixed, size, min, extra } = consts;
    // An invariant runs on a value after its structure (and the invariants
    // of what it holds) was accepted, and before any byte of it is returned.
    let check = |value: Tokens| match &opts.invariant {
        Some(path) => quote! {
            if let ::core::result::Result::Err(__why) = #path(#value) {
                return ::core::result::Result::Err(#k::pssz::PsszError(::core::convert::Into::into(__why)));
            }
        },
        None => quote!(),
    };
    if let Some(path) = &opts.invariant {
        let checked = check(quote!(&__value));
        let _ = path;
        // A value that violates its invariant is not a value of the type:
        // a pruned trailing optional is checked like a stored one.
        if !missing_optional.is_empty() {
            if let Data::Struct(data) = &input.data {
                let ty = field_info(k, &data.fields)?.remove(0).1.clone();
                let bound = field_bounds(k, &data.fields)?.remove(0);
                let construct = attrs::construct(quote!(Self), &data.fields, &[decode_value(k, &bound, quote!(__inner))])?;
                missing_optional = quote! {
                    fn pssz_missing_optional() -> #k::pssz::PsszResult<Self> {
                        let __inner = <#ty as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_missing_optional()?;
                        let __value = #construct;
                        #checked
                        ::core::result::Result::Ok(__value)
                    }
                };
            }
        }
    }
    let unpack_check = check(quote!(&__value));
    let pack_check = match &opts.invariant {
        Some(path) => quote! {
            if #k::pssz_checked::writer_checks_invariants() {
                if let ::core::result::Result::Err(__why) = #path(self) {
                    return ::core::result::Result::Err(#k::pssz::PsszError(::core::convert::Into::into(__why)));
                }
            }
        },
        None => quote!(),
    };
    Ok(if unpack {
        // With an invariant, validation materialises the value: the decoder
        // is the validator (same structure checks, then the invariant).
        let validation = if opts.invariant.is_some() {
            quote! {
                impl #impl_g #k::pssz::PsszValidate<__PsszFormat> for #name #type_g #where_c {
                    fn pssz_validate(bytes: &[u8]) -> #k::pssz::PsszResult<()> {
                        <Self as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_unpack(bytes).map(|_| ())
                    }
                }
            }
        } else {
            quote! {
                impl #validation_g #k::pssz::PsszValidate<__PsszFormat> for #name #type_g #validation_where {
                    #[allow(unused_variables)]
                    fn pssz_validate(bytes: &[u8]) -> #k::pssz::PsszResult<()> {
                        #depth_guard
                        #validation_body
                    }
                }
            }
        };
        let trusted = match &trusted_body {
            Some(body) => quote! {
                #[allow(unused_variables, unreachable_code)]
                fn pssz_unpack_trusted(bytes: &[u8]) -> Self {
                    #body
                }
            },
            None => quote!(),
        };
        quote! {
            impl #impl_g #k::pssz::PsszUnpack<__PsszFormat> for #name #type_g #where_c {
                #missing_optional
                #[allow(unused_variables)]
                fn pssz_unpack(bytes: &[u8]) -> #k::pssz::PsszResult<Self> {
                    #depth_guard
                    let __value: Self = (|| -> #k::pssz::PsszResult<Self> { #body })()?;
                    #unpack_check
                    ::core::result::Result::Ok(__value)
                }
                #trusted
            }
            #validation
        }
    } else {
        quote! {
            impl #impl_g #k::pssz::PsszPack<__PsszFormat> for #name #type_g #where_c {
                #extra
                const MIN_ENCODED_SIZE: usize = #min;
                const IS_FIXED_SIZE: bool = #fixed;
                const FIXED_SIZE: usize = if <Self as #k::pssz::PsszPack<__PsszFormat>>::IS_FIXED_SIZE { #size } else { 0 };
                fn pssz_size(&self) -> usize {
                    let mut out = ::std::vec::Vec::new();
                    <Self as #k::pssz::PsszPack<__PsszFormat>>::pssz_pack_checked(self, &mut out).expect("pssz: encode failed; use try_to_pssz");
                    out.len()
                }
                fn pssz_pack(&self, out: &mut ::std::vec::Vec<u8>) { <Self as #k::pssz::PsszPack<__PsszFormat>>::pssz_pack_checked(self, out).expect("pssz: encode failed; use try_to_pssz"); }
                #[allow(unused_variables)]
                fn pssz_pack_checked(&self, destination: &mut ::std::vec::Vec<u8>) -> #k::pssz::PsszResult<()> {
                    #encode_depth_guard
                    let mut temporary = ::std::vec::Vec::new();
                    let out = &mut temporary;
                    let result: #k::pssz::PsszResult<()> = (|| { #body })();
                    result?;
                    #pack_check
                    destination.try_reserve(temporary.len()).map_err(|_| #k::pssz::PsszError::new("pssz: output allocation overflow"))?;
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

fn schema_attribute(k: &syn::Path, name: &str, value: &str) -> Tokens {
    quote! { #k::pssz::schema_ir::Attribute { name: #name.into(), value: ::core::option::Option::Some(#value.into()) } }
}

/// The member attributes `jsonInt` / `ijsonInt` for `#[json(json_string)]`
/// and friends on the field, else on the type (`defaults`).
fn schema_int_forms(k: &syn::Path, field: &syn::Field, defaults: Forms) -> syn::Result<Vec<Tokens>> {
    let attrs = attrs::field(field)?;
    let text = |string: bool| if string { "string" } else { "number" };
    let mut out = Vec::new();
    if let Some(form) = attrs.forms.json.or(defaults.json) {
        out.push(schema_attribute(k, "jsonInt", text(form)));
    }
    if let Some(form) = attrs.forms.ijson.or(defaults.ijson) {
        out.push(schema_attribute(k, "ijsonInt", text(form)));
    }
    Ok(out)
}

fn schema_record(k: &syn::Path, fields: &Fields, dwnc: bool, cap: Option<u64>, forms: Forms, case: Option<Case>) -> syn::Result<Tokens> {
    let members = attrs::members(fields)?;
    // The member's name in text: its identifier, `#[psio(name)]` /
    // `#[serde(rename)]`, or the identifier under the `rename_all` convention.
    let names: Vec<_> = members
        .iter()
        .map(|m| match &m.member {
            Member::Named(_) => attrs::member_name(m.field, &m.attrs, case),
            Member::Unnamed(index) => index.index.to_string(),
        })
        .collect();
    for (i, name) in names.iter().enumerate() {
        if names[..i].contains(name) {
            return Err(syn::Error::new_spanned(members[i].field, "duplicate member name"));
        }
    }
    // A member with bounds is described as the type its annotation says.
    let types: Vec<_> = field_info(k, fields)?.into_iter().map(|(_, ty)| ty).collect();
    let member_attributes = members
        .iter()
        .map(|m| schema_int_forms(k, m.field, forms))
        .collect::<syn::Result<Vec<_>>>()?;
    // The order C++ SchemaBuilder emits.
    let mut attributes = Vec::new();
    if dwnc {
        attributes.push(schema_attribute(k, "definitionWillNotChange", ""));
    }
    if let Some(cap) = cap {
        attributes.push(schema_attribute(k, "maxDynamicData", &cap.to_string()));
    }
    Ok(quote! {
        #k::pssz::schema_ir::AnyType::new(#k::pssz::schema_ir::AnyValue::Object(#k::pssz::schema_ir::Object {
            members: ::std::vec![#(#k::pssz::schema_ir::Member {
                name: #names.into(), r#type: ::std::boxed::Box::new(<#types as #k::pssz::schema_ir::ToPsszSchema>::schema(builder)?),
                attributes: {
                    #[allow(unused_mut)]
                    let mut __attributes: ::std::vec::Vec<#k::pssz::schema_ir::Attribute> = ::std::vec![#(#member_attributes),*];
                    __attributes.extend(<#types as #k::pssz::schema_ir::ToPsszSchema>::member_attributes());
                    __attributes
                }
            }),*], attributes: ::std::vec![#(#attributes),*]
        }))
    })
}

fn expand_schema(input: DeriveInput) -> syn::Result<Tokens> {
    let opts = options(&input, Derive::Schema)?;
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
    let c_enum = c_like(&input, &opts)?;
    let body = match &input.data {
        // An enum is an Enum over its underlying integer: the wire form plus
        // the enumerator names and values (C++ PSIO_REFLECT_ENUM). The names
        // are the JSON names; the catch-all is not an enumerator.
        Data::Enum(_) if c_enum.is_some() => {
            let shape = c_enum.as_ref().unwrap();
            let repr = shape.repr.as_ref().unwrap();
            let (preamble, values) = shape.values();
            let names = shape
                .declared
                .iter()
                .map(|v| Ok(attrs::variant_name(v, &attrs::variant(v)?, opts.rename_all)))
                .collect::<syn::Result<Vec<String>>>()?;
            quote! {{
                #preamble
                #k::pssz::schema_ir::AnyType::new(#k::pssz::schema_ir::AnyValue::Enum(#k::pssz::schema_ir::Enum {
                    r#type: ::std::boxed::Box::new(<#repr as #k::pssz::schema_ir::ToPsszSchema>::build_schema(builder)?),
                    // The value sign-extended to 64 bits.
                    members: ::std::vec![#(#k::pssz::schema_ir::Enumerator {
                        name: #names.into(), value: (#values) as i64 as u64
                    }),*],
                }))
            }}
        }
        Data::Struct(data) if opts.transparent => {
            let info = field_info(k, &data.fields)?;
            if info.len() != 1 {
                return Err(syn::Error::new_spanned(
                    &data.fields,
                    "transparent PSSZ structs require exactly one member",
                ));
            }
            let ty = info[0].1;
            if opts.custom.is_some() || opts.invariant.is_some() {
                // The wire type inline under the Custom node, as the C++
                // builder writes a strong typedef.
                quote! { <#ty as #k::pssz::schema_ir::ToPsszSchema>::build_schema(builder)? }
            } else {
                quote! { <#ty as #k::pssz::schema_ir::ToPsszSchema>::schema(builder)? }
            }
        }
        Data::Struct(data) => {
            schema_record(k, &data.fields, opts.dwnc, opts.cap, opts.forms, opts.rename_all)?
        }
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
                let variant_attrs = attrs::variant(variant)?;
                let ty = match &variant.fields {
                    Fields::Unit => {
                        quote! { #k::pssz::schema_ir::AnyType::new(#k::pssz::schema_ir::AnyValue::Struct(#k::pssz::schema_ir::Struct { members: ::std::vec![], attributes: ::std::vec![] })) }
                    }
                    Fields::Unnamed(fields) if fields.unnamed.len() == 1 => {
                        let _ = fields;
                        let ty = field_info(k, &variant.fields)?.remove(0).1;
                        quote! { <#ty as #k::pssz::schema_ir::ToPsszSchema>::schema(builder)? }
                    }
                    Fields::Unnamed(fields) => {
                        let _ = fields;
                        let types: Vec<_> = field_info(k, &variant.fields)?.into_iter().map(|(_, ty)| ty).collect();
                        quote! {
                            #k::pssz::schema_ir::AnyType::new(#k::pssz::schema_ir::AnyValue::Tuple(#k::pssz::schema_ir::Tuple { members: ::std::vec![#(::std::boxed::Box::new(<#types as #k::pssz::schema_ir::ToPsszSchema>::schema(builder)?)),*] }))
                        }
                    }
                    _ => schema_record(
                        k,
                        &variant.fields,
                        false,
                        None,
                        Forms::default(),
                        variant_attrs.rename_all.or(opts.rename_all_fields),
                    )?,
                };
                // The I-JSON form names an alternative by `#[psio(name)]` /
                // `#[serde(rename)]`, or by the enum's `rename_all`.
                let text_name = attrs::variant_name(variant, &variant_attrs, opts.rename_all);
                let attributes: Vec<_> = Some(&text_name)
                    .filter(|name| **name != variant_name)
                    .iter()
                    .map(|name| schema_attribute(k, "ijsonName", name))
                    .collect();
                members.push(quote! { #k::pssz::schema_ir::Member { name: #variant_name.into(), r#type: ::std::boxed::Box::new(#ty), attributes: ::std::vec![#(#attributes),*] } });
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
    // A custom type (a text form, an invariant, or both): Custom { the wire
    // type, id }.
    let custom = match (&opts.custom, &opts.invariant) {
        (Some(id), _) => Some(id.clone()),
        (None, Some(_)) => Some(None),
        (None, None) => None,
    };
    // The id defaults to the schema name: a type whose name is a reserved
    // id needs an id of its own.
    if let Some(None) = &custom {
        let implied = opts.name.clone().unwrap_or_else(|| name.to_string());
        if input.generics.params.is_empty() && attrs::reserved_custom_id(&implied) {
            return Err(syn::Error::new_spanned(
                name,
                format!(
                    "the custom id of this type would be its schema name, \"{implied}\", which is an id \
                     PSIO's own types have in a schema; give it one of its own: #[psio(custom = \"Id\")]"
                ),
            ));
        }
    }
    let (body, custom_id) = match &custom {
        None => (body, quote!()),
        Some(id) => {
            let id = match id {
                Some(id) => quote!(::std::string::String::from(#id)),
                None => quote!(<Self as #k::pssz::schema_ir::ToPsszSchema>::schema_name()
                    .expect("a derived type has a schema name")),
            };
            (
                quote! {
                    #k::pssz::schema_ir::AnyType::new(#k::pssz::schema_ir::AnyValue::Custom(#k::pssz::schema_ir::Custom {
                        r#type: ::std::boxed::Box::new(#body),
                        id: #id,
                    }))
                },
                quote! {
                    fn custom_id() -> ::core::option::Option<::std::string::String> {
                        ::core::option::Option::Some(#id)
                    }
                },
            )
        }
    };
    Ok(quote! {
        impl #impl_g #k::pssz::schema_ir::ToPsszSchema for #name #type_g #where_c {
            fn schema_name() -> ::core::option::Option<::std::string::String> { #schema_name }
            #custom_id
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
    let opts = options(&input, Derive::Pssz)?;
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
    let mut_accessor = format_ident!("{}PsszMutAccessors", name);
    let mut mut_declarations = Vec::new();
    let mut mut_methods = Vec::new();
    match &input.data {
        Data::Struct(data) => {
            let info = field_info(k, &data.fields)?;
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
                let try_method = format_ident!("try_{}", method.to_string().trim_start_matches("r#"));
                declarations.push(quote!(fn #method(&self) -> #k::pssz_view::PsszView<'__pssz_view, #ty, __PsszFormat>;));
                declarations.push(quote!(
                    /// Fallible form for unvalidated buffers.
                    fn #try_method(&self) -> #k::pssz::PsszResult<#k::pssz_view::PsszView<'__pssz_view, #ty, __PsszFormat>>;
                ));
                methods.push(quote! { fn #method(&self) -> #k::pssz_view::PsszView<'__pssz_view, #ty, __PsszFormat> { self.field::<#index>() } });
                methods.push(quote! { fn #try_method(&self) -> #k::pssz::PsszResult<#k::pssz_view::PsszView<'__pssz_view, #ty, __PsszFormat>> { self.try_field::<#index>() } });
                let bare = method.to_string().trim_start_matches("r#").to_string();
                let mut_method = format_ident!("{}_mut", bare);
                let try_mut_method = format_ident!("try_{}_mut", bare);
                mut_declarations.push(quote!(
                    /// The field for writing; panics when it is not stored in the frame.
                    fn #mut_method(&mut self) -> #k::pssz_view::PsszMutView<'_, #ty, __PsszFormat>;
                    fn #try_mut_method(&mut self) -> #k::pssz::PsszResult<#k::pssz_view::PsszMutView<'_, #ty, __PsszFormat>>;
                ));
                mut_methods.push(quote! {
                    fn #mut_method(&mut self) -> #k::pssz_view::PsszMutView<'_, #ty, __PsszFormat> { self.field_mut::<#index>() }
                    fn #try_mut_method(&mut self) -> #k::pssz::PsszResult<#k::pssz_view::PsszMutView<'_, #ty, __PsszFormat>> { self.try_field_mut::<#index>() }
                });
                let dwnc = opts.dwnc;
                let transparent = opts.transparent;
                field_impls.push(quote! {
                    impl #field_g #k::pssz_view::PsszField<__PsszFormat, #index> for #name #type_g #field_where {
                        type Field = #ty;
                        const LOCATION: #k::pssz_checked::FieldLocation = #k::pssz_checked::record_location(
                            <Self as #k::pssz::PsszPack<__PsszFormat>>::OFFSET_WIDTH, &[
                            #(#k::pssz_checked::Layout::of::<__PsszFormat, #types>()),*
                        ], #index);
                        const DWNC: bool = #dwnc;
                        const TRANSPARENT: bool = #transparent;
                    }
                });
            }
        }
        Data::Enum(_) if c_like(&input, &opts)?.is_some() => {
            let repr = c_like(&input, &opts)?.unwrap().repr.unwrap();
            declarations.push(quote!(
                /// The underlying integer; panics on a malformed unvalidated view.
                fn raw_value(&self) -> #repr;
            ));
            methods.push(quote!(
                fn raw_value(&self) -> #repr {
                    let mut buffer = [0u8; ::core::mem::size_of::<#repr>()];
                    buffer.copy_from_slice(&self.bytes()[..::core::mem::size_of::<#repr>()]);
                    <#repr>::from_le_bytes(buffer)
                }
            ));
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
                    self.bytes()[0]
                }
            ));
            for (index, variant) in data.variants.iter().enumerate() {
                let method = format_ident!("as_{}", variant.ident);
                let info = field_info(k, &variant.fields)?;
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
                        if self.bytes()[0] == #tag { ::core::option::Option::Some(#k::pssz_view::PsszView::trusted(&self.bytes()[1..])) } else { ::core::option::Option::None }
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
        #[allow(non_snake_case)]
        #vis trait #mut_accessor #impl_g #where_c { #(#mut_declarations)* }
        #[allow(non_snake_case)]
        impl #impl_g #mut_accessor #trait_g for #k::pssz_view::PsszMutView<'__pssz_view, #name #type_g, __PsszFormat> #where_c { #(#mut_methods)* }
    })
}
pub fn derive_view(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    expand_view(input)
        .unwrap_or_else(syn::Error::into_compile_error)
        .into()
}
