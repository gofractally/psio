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
    /// `#[psio(max_fields = N)]`: the field-count cap.
    max_fields: Option<u64>,
    /// `#[psio(arguments = ...)]`: a method's argument list; `Some(true)`
    /// when its parameters have names.
    arguments: Option<bool>,
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
    /// `#[psio(untagged)]` / `#[serde(untagged)]`: the Variant node carries
    /// the attribute `untagged`.
    untagged: bool,
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
        max_fields: item.max_fields,
        arguments: item.arguments,
        variant: item.variant,
        custom: item.custom,
        invariant: item.invariant,
        forms: item.forms,
        rename_all: item.rename_all,
        rename_all_fields: item.rename_all_fields,
        untagged: item.untagged,
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

/// The format of a member's position (docs/pssz-spec.md 3.6): beneath a
/// record with a cap, beneath a record without one, or the position of the
/// value that holds it (a transparent type, a tuple, a variant).
#[derive(Clone, Copy, PartialEq)]
enum Position {
    Under(u64),
    Free,
    Same,
}
impl Position {
    fn record(cap: Option<u64>) -> Self {
        cap.map_or(Position::Free, Position::Under)
    }
    fn format(self, k: &syn::Path) -> Tokens {
        match self {
            Position::Under(cap) => {
                let cap = cap as usize;
                quote!(<__PsszFormat as #k::pssz::PsszFormat>::Under<#cap>)
            }
            Position::Free => quote!(<__PsszFormat as #k::pssz::PsszFormat>::Free),
            Position::Same => quote!(__PsszFormat),
        }
    }
}
/// The position of the members of the item: of a struct's members, or of the
/// fields of each enum variant (the fields of a struct-like variant are the
/// members of a record without a cap; the others are a payload or a tuple).
fn positions(input: &DeriveInput, opts: &Options) -> Vec<Position> {
    match &input.data {
        Data::Struct(_) if opts.transparent => vec![Position::Same],
        Data::Struct(_) => vec![Position::record(opts.cap)],
        Data::Enum(e) => e
            .variants
            .iter()
            .map(|v| if matches!(v.fields, Fields::Named(_)) { Position::Free } else { Position::Same })
            .collect(),
        Data::Union(_) => vec![],
    }
}

/// The types of the fields that name a generic parameter, each with the
/// format of its position.
fn generic_fields(input: &DeriveInput, opts: &Options) -> Vec<(Type, Tokens)> {
    let k = &opts.krate;
    let positions = positions(input, opts);
    let names: Vec<_> = input
        .generics
        .type_params()
        .map(|p| p.ident.to_string())
        .collect();
    let own_name = input.ident.to_string();
    let fields: Vec<(&syn::Field, Position)> = match &input.data {
        Data::Struct(s) => s.fields.iter().map(|f| (f, positions[0])).collect(),
        Data::Enum(e) => e
            .variants
            .iter()
            .zip(&positions)
            .flat_map(|(v, position)| v.fields.iter().map(move |f| (f, *position)))
            .collect(),
        Data::Union(_) => vec![],
    };
    fields
        .into_iter()
        // A skipped field is not encoded, so its type needs no codec.
        .filter(|(field, _)| !attrs::field(field).is_ok_and(|a| a.skip))
        .filter_map(|(field, position)| {
            let ty = &field.ty;
            let text = quote!(#ty).to_string();
            let parts: Vec<_> = text
                .split(|c: char| !c.is_alphanumeric() && c != '_')
                .collect();
            if !parts.contains(&own_name.as_str())
                && names.iter().any(|n| parts.contains(&n.as_str()))
            {
                Some((ty.clone(), position.format(k)))
            } else {
                None
            }
        })
        .collect()
}

/// Width / cap of the record being generated: the struct itself, or for an
/// enum alternative with several fields the equivalent positional tuple.
///
/// `members` is the format of the members' position: a record sets the
/// position cap for what lies beneath it (its own cap, or none); a tuple
/// leaves the position as it is.
struct Frame { width: Tokens, cap: Tokens, members: Tokens }
fn self_frame(k: &syn::Path, cap: Option<u64>) -> Frame {
    Frame {
        width: quote!(<Self as #k::pssz::PsszPack<__PsszFormat>>::OFFSET_WIDTH),
        cap: quote!(<Self as #k::pssz::PsszPack<__PsszFormat>>::MAX_DYNAMIC_DATA),
        members: Position::record(cap).format(k),
    }
}
/// The fields of an enum variant: a tuple at the variant's position, or for
/// a struct-like variant a record without a cap (whose width is its own).
fn tuple_frame(k: &syn::Path, types: &[&Type], position: Position) -> Frame {
    let members = position.format(k);
    Frame {
        width: quote!(<(#(#types,)*) as #k::pssz::PsszPack<#members>>::OFFSET_WIDTH),
        cap: quote!(::core::option::Option::None),
        members,
    }
}

fn record_encode(k: &syn::Path, types: &[&Type], values: &[Tokens], dwnc: bool, frame: &Frame) -> Tokens {
    let indices: Vec<_> = (0..types.len()).collect();
    let (width, cap, m) = (&frame.width, &frame.cap, &frame.members);
    // The frame is written where it belongs in `out` (`RecordEncoder`):
    // the fields in declaration order, each kept one into its slot or
    // behind the fixed region. `__depth` is the depth of the record's node.
    //
    // The number of kept fields: all of a fixed layout, else everything up
    // to the last field that is not an absent optional. Written as a chain
    // from the last field, so it is a constant up to the first field that
    // is not an `Option`, and so is the size of the fixed region.
    let mut keep = quote!(0usize);
    for (index, (ty, value)) in types.iter().zip(values).enumerate() {
        let kept = index + 1;
        keep = quote! {
            if <#ty as #k::pssz::PsszPack<#m>>::IS_OPTIONAL && #k::pssz::PsszPack::<#m>::pssz_is_none(#value) {
                #keep
            } else {
                #kept
            }
        };
    }
    let count = types.len();
    quote! {
        let __keep: usize = if #dwnc { #count } else { #keep };
        let __width: usize = #width;
        let __fixed = 0usize #(.saturating_add(
            if #indices < __keep { #k::pssz_checked::slot::<#m, #types>(__width) } else { 0 }
        ))*;
        #[allow(unused_mut)]
        let mut __record = #k::pssz_checked::RecordEncoder::begin(out, __width, #dwnc, __fixed, __depth)?;
        #(if #indices < __keep { __record.field::<#m, _>(out, #values)?; })*
        __record.finish::<__PsszFormat>(out, #cap)
    }
}

/// A fixed-size record written into its slot: every field is fixed-size and
/// kept, at a constant position. Same checks, in the same order, as
/// `record_encode`.
fn record_encode_fixed(k: &syn::Path, types: &[&Type], values: &[Tokens], frame: &Frame) -> Tokens {
    let (cap, m) = (&frame.cap, &frame.members);
    quote! {
        #[allow(unused_mut)]
        let mut __at = 0usize;
        #({
            let __size = <#types as #k::pssz::PsszPack<#m>>::FIXED_SIZE;
            let __end = __at.wrapping_add(__size);
            let __slot = slot.get_mut(__at..__end).ok_or(#k::pssz::PsszError::new("pssz: fixed encoder size mismatch"))?;
            #k::pssz::PsszPack::<#m>::pssz_pack_fixed(#values, __slot, __depth + 1)?;
            __at = __end;
        })*
        let _ = __at;
        #k::pssz_checked::wire_span(slot.len())?;
        if <__PsszFormat as #k::pssz::PsszFormat>::AUTO {
            #k::pssz_checked::check_cap(#cap, slot.len())?;
        }
        ::core::result::Result::Ok(())
    }
}

/// The size pass of `record_encode`: the same checks in the same order
/// (`pssz_checked::RecordSize`), and the size of the frame.
fn record_size(k: &syn::Path, types: &[&Type], values: &[Tokens], dwnc: bool, frame: &Frame) -> Tokens {
    let indices: Vec<_> = (0..types.len()).collect();
    let (width, cap, m) = (&frame.width, &frame.cap, &frame.members);
    let mut keep = quote!(0usize);
    for (index, (ty, value)) in types.iter().zip(values).enumerate() {
        let kept = index + 1;
        keep = quote! {
            if <#ty as #k::pssz::PsszPack<#m>>::IS_OPTIONAL && #k::pssz::PsszPack::<#m>::pssz_is_none(#value) {
                #keep
            } else {
                #kept
            }
        };
    }
    let count = types.len();
    quote! {
        let __keep: usize = if #dwnc { #count } else { #keep };
        let __width: usize = #width;
        let __fixed = 0usize #(.saturating_add(
            if #indices < __keep { #k::pssz_checked::slot::<#m, #types>(__width) } else { 0 }
        ))*;
        #[allow(unused_mut)]
        let mut __record = #k::pssz_checked::RecordSize::begin(__width, #dwnc, __fixed, __depth);
        #(if #indices < __keep { __record.field::<#m, _>(#values)?; })*
        __record.finish::<__PsszFormat>(#cap)
    }
}

/// The size pass of `record_encode_fixed`.
fn record_size_fixed(k: &syn::Path, values: &[Tokens], frame: &Frame) -> Tokens {
    let (cap, m) = (&frame.cap, &frame.members);
    quote! {
        #(#k::pssz::PsszPack::<#m>::pssz_size_fixed(#values, __depth + 1)?;)*
        let __size = <Self as #k::pssz::PsszPack<__PsszFormat>>::FIXED_SIZE;
        #k::pssz_checked::wire_span(__size)?;
        if <__PsszFormat as #k::pssz::PsszFormat>::AUTO {
            #k::pssz_checked::check_cap(#cap, __size)?;
        }
        ::core::result::Result::Ok(())
    }
}

/// `pssz_size_hint` of a record frame: the header and each field's slot and
/// payload (a pruned trailing optional is counted; the hint is an estimate).
fn record_hint(k: &syn::Path, values: &[Tokens], dwnc: bool, frame: &Frame) -> Tokens {
    let (width, m) = (&frame.width, &frame.members);
    quote! {{
        let __width: usize = #width;
        #[allow(unused_mut)]
        let mut __total: usize = if #dwnc { 0 } else { __width };
        #(__total = __total.saturating_add(#k::pssz_checked::field_size_hint::<#m, _>(__width, #values));)*
        __total
    }}
}

/// The check of the frame of a fixed-size record, whose fields then lie at
/// constant positions (`pssz_checked::fixed_record_frame`).
fn fixed_frame(k: &syn::Path, types: &[&Type], frame: &Frame) -> Tokens {
    let (width, m) = (&frame.width, &frame.members);
    quote! {
        #k::pssz_checked::fixed_record_frame(
            #width,
            <Self as #k::pssz::PsszPack<__PsszFormat>>::FIXED_SIZE,
            bytes.len(),
            false #(|| (<#types as #k::pssz::PsszPack<#m>>::IS_OPTIONAL && <#types as #k::pssz::PsszPack<#m>>::IS_FIXED_SIZE))*,
        )?;
    }
}

/// The checked frame of a record with variable fields
/// (`pssz_checked::RecordFrame`), written out field by field so that the
/// record's layout is constants of the generated reader: `__pssz_start_i` is
/// where field `i` starts (or `ABSENT`) and `__pssz_end_i` where it ends when
/// it is variable. The same checks, in the same order, as
/// `pssz_checked::record_spans_into`.
fn record_frame(k: &syn::Path, types: &[&Type], dwnc: bool, frame: &Frame) -> (Tokens, Vec<syn::Ident>, Vec<syn::Ident>) {
    let (width, m) = (&frame.width, &frame.members);
    let starts: Vec<_> = (0..types.len()).map(|i| format_ident!("__pssz_start_{i}")).collect();
    let ends: Vec<_> = (0..types.len()).map(|i| format_ident!("__pssz_end_{i}")).collect();
    // A variable field ends where the next present variable field starts:
    // from the last field back.
    let (rev_types, rev_starts, rev_ends): (Vec<_>, Vec<_>, Vec<_>) = {
        let mut all: Vec<_> = types.iter().zip(&starts).zip(&ends).map(|((t, s), e)| (*t, s.clone(), e.clone())).collect();
        all.reverse();
        (all.iter().map(|x| x.0).collect(), all.iter().map(|x| x.1.clone()).collect(), all.iter().map(|x| x.2.clone()).collect())
    };
    let tokens = quote! {
        let __width: usize = #width;
        let __known = ::core::option::Option::Some(0usize);
        #(let __known = #k::pssz_checked::add_slot::<#m, #types>(__known, __width);)*
        #[allow(unused_mut)]
        let mut __frame = #k::pssz_checked::RecordFrame::begin(
            __width,
            __known,
            false #(|| (<#types as #k::pssz::PsszPack<#m>>::IS_OPTIONAL && <#types as #k::pssz::PsszPack<#m>>::IS_FIXED_SIZE))*,
            true #(&& <#types as #k::pssz::PsszPack<#m>>::IS_FIXED_SIZE)*,
            #dwnc,
            bytes,
        )?;
        #(let #starts: usize = __frame.field::<#m, #types>(bytes)?;)*
        #[allow(unused_mut)]
        let mut __end: usize = __frame.finish(bytes)?;
        #(
            let #rev_ends: usize = __end;
            if !<#rev_types as #k::pssz::PsszPack<#m>>::IS_FIXED_SIZE && #rev_starts != #k::pssz_checked::ABSENT {
                __end = #rev_starts;
            }
        )*
        let _ = __end;
    };
    (tokens, starts, ends)
}

/// `record`: the frame is the item's own (a struct), which may be
/// fixed-size; an enum alternative's never is. `__depth` is the depth of the
/// record's node.
fn record_decode(k: &syn::Path, fields: &Fields, construct: Tokens, dwnc: bool, frame: &Frame, record: bool) -> syn::Result<Tokens> {
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
    let (cap, m) = (&frame.cap, &frame.members);
    let fixed = if record {
        let check = fixed_frame(k, &types, frame);
        quote! {
            if <Self as #k::pssz::PsszPack<__PsszFormat>>::IS_FIXED_SIZE {
                #check
                #[allow(unused_mut)]
                let mut __at = 0usize;
                #(let #values = {
                    let __size = <#types as #k::pssz::PsszPack<#m>>::FIXED_SIZE;
                    let __value = <#types as #k::pssz::PsszUnpack<#m>>::pssz_unpack_at_depth(&bytes[__at..__at + __size], __depth + 1)?;
                    __at += __size;
                    __value
                };)*
                let _ = __at;
                return ::core::result::Result::Ok(#initializer);
            }
        }
    } else {
        quote!()
    };
    let (resolve, starts, ends) = record_frame(k, &types, dwnc, frame);
    Ok(quote! {
        #k::pssz_checked::enter_record::<__PsszFormat>(#cap, bytes)?;
        #fixed
        #resolve
        #(let #values = #k::pssz_checked::decode_frame_field::<#m, #types>(bytes, #starts, #ends, __depth + 1)?;)*
        ::core::result::Result::Ok(#initializer)
    })
}

/// How a member is read from bytes the caller vouches for: by the trusted
/// reader of its own type where its annotation leaves the bytes alone (the
/// bounds are not checked), else through the described type.
///
/// `m` is the format of the member's position.
fn trusted_value(k: &syn::Path, m: &Tokens, ty: &Type, bound: &Option<attrs::Annotated>, bytes: Tokens) -> Tokens {
    trusted_as(k, bound, quote!(<#ty as #k::pssz::PsszUnpack<#m>>::pssz_unpack_trusted(#bytes)))
}
/// A member read as its described type, as the member's own type.
fn trusted_as(k: &syn::Path, bound: &Option<attrs::Annotated>, value: Tokens) -> Tokens {
    match bound {
        Some(attrs::Annotated { own, spec }) => quote! {
            <#own as #k::pssz::bounds::Bounds<#spec>>::from_as(#value)
        },
        None => value,
    }
}

/// The trusted reader of a record: no depth, cap, bound or invariant
/// check, and nothing of the frame is compared with anything
/// (`pssz_checked::TrustedFrame`); a fixed-size record is read at constant
/// offsets. `record`: as for `record_decode`.
fn record_decode_trusted(k: &syn::Path, fields: &Fields, construct: Tokens, dwnc: bool, frame: &Frame, record: bool) -> syn::Result<Tokens> {
    let info = field_info(k, fields)?;
    let bounds = field_bounds(k, fields)?;
    let types: Vec<_> = info.iter().map(|(_, ty)| *ty).collect();
    let values: Vec<_> = (0..info.len()).map(|i| format_ident!("__pssz_field_{i}")).collect();
    let initializer =
        attrs::construct(construct, fields, &values.iter().map(|v| quote!(#v)).collect::<Vec<_>>())?;
    let m = &frame.members;
    let fixed_reads: Vec<_> = types
        .iter()
        .zip(&bounds)
        .map(|(ty, bound)| trusted_value(k, m, ty, bound, quote!(&bytes[__at..__at + __size])))
        .collect();
    let starts: Vec<_> = (0..types.len()).map(|i| format_ident!("__pssz_start_{i}")).collect();
    let ends: Vec<_> = (0..types.len()).map(|i| format_ident!("__pssz_end_{i}")).collect();
    let reads: Vec<_> = types
        .iter()
        .zip(&bounds)
        .zip(starts.iter().zip(&ends))
        .map(|((ty, bound), (start, end))| {
            trusted_as(k, bound, quote!(#k::pssz_checked::trusted_frame_field::<#m, #ty>(bytes, #start, #end)))
        })
        .collect();
    let mut rev: Vec<_> = types.iter().zip(&starts).zip(&ends).map(|((t, s), e)| (*t, s.clone(), e.clone())).collect();
    rev.reverse();
    let rev_types: Vec<_> = rev.iter().map(|x| x.0).collect();
    let rev_starts: Vec<_> = rev.iter().map(|x| x.1.clone()).collect();
    let rev_ends: Vec<_> = rev.iter().map(|x| x.2.clone()).collect();
    let width = &frame.width;
    let fixed = if record {
        quote! {
            if <Self as #k::pssz::PsszPack<__PsszFormat>>::IS_FIXED_SIZE {
                #[allow(unused_mut)]
                let mut __at = 0usize;
                #(let #values = {
                    let __size = <#types as #k::pssz::PsszPack<#m>>::FIXED_SIZE;
                    let __value = #fixed_reads;
                    __at += __size;
                    __value
                };)*
                let _ = __at;
                return #initializer;
            }
        }
    } else {
        quote!()
    };
    Ok(quote! {
        #fixed
        let __width: usize = #width;
        let __known = 0usize #(.saturating_add(#k::pssz_checked::slot::<#m, #types>(__width)))*;
        #[allow(unused_mut)]
        let mut __frame = #k::pssz_checked::TrustedFrame::begin(__width, __known, #dwnc, bytes);
        #(let #starts: usize = __frame.field::<#m, #types>(bytes);)*
        #[allow(unused_mut)]
        let mut __end: usize = __frame.finish(__known, bytes);
        #(
            let #rev_ends: usize = __end;
            if !<#rev_types as #k::pssz::PsszPack<#m>>::IS_FIXED_SIZE && #rev_starts != #k::pssz_checked::ABSENT {
                __end = #rev_starts;
            }
        )*
        let _ = __end;
        #(let #values = #reads;)*
        #initializer
    })
}

fn record_validate(k: &syn::Path, fields: &Fields, dwnc: bool, frame: &Frame, record: bool) -> syn::Result<Tokens> {
    let info = field_info(k, fields)?;
    let types: Vec<_> = info.iter().map(|(_, ty)| *ty).collect();
    let (cap, m) = (&frame.cap, &frame.members);
    let fixed = if record {
        let check = fixed_frame(k, &types, frame);
        quote! {
            if <Self as #k::pssz::PsszPack<__PsszFormat>>::IS_FIXED_SIZE {
                #check
                #[allow(unused_mut)]
                let mut __at = 0usize;
                #({
                    let __size = <#types as #k::pssz::PsszPack<#m>>::FIXED_SIZE;
                    <#types as #k::pssz::PsszValidate<#m>>::pssz_validate_at_depth(&bytes[__at..__at + __size], __depth + 1)?;
                    __at += __size;
                })*
                let _ = __at;
                return ::core::result::Result::Ok(());
            }
        }
    } else {
        quote!()
    };
    let (resolve, starts, ends) = record_frame(k, &types, dwnc, frame);
    Ok(quote! {
        #k::pssz_checked::enter_record::<__PsszFormat>(#cap, bytes)?;
        #fixed
        #resolve
        #(#k::pssz_checked::validate_frame_field::<#m, #types>(bytes, #starts, #ends, __depth + 1)?;)*
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
    for (ty, m) in generic_fields(&input, &opts) {
        generics.make_where_clause().predicates.push(if unpack {
            parse_quote!(#ty: #k::pssz::PsszPack<#m> + #k::pssz::PsszUnpack<#m>)
        } else {
            parse_quote!(#ty: #k::pssz::PsszPack<#m>)
        });
    }
    let (impl_g, _, where_c) = generics.split_for_impl();
    let mut validation_generics = input.generics.clone();
    validation_generics
        .params
        .push(parse_quote!(__PsszFormat: #k::pssz::PsszFormat));
    for (ty, m) in generic_fields(&input, &opts) {
        validation_generics.make_where_clause().predicates.push(parse_quote!(#ty: #k::pssz::PsszPack<#m> + #k::pssz::PsszValidate<#m>));
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
    // The writer of a fixed-size value into its slot, and the size hint,
    // where the derive has them (uses `slot` / `self`, and `__depth`).
    let mut fixed_body: Option<Tokens> = None;
    let mut hint_body: Option<Tokens> = None;
    // The size passes of `body` and of `fixed_body` (writers only).
    let mut size_body: Option<Tokens> = None;
    let mut size_fixed_body: Option<Tokens> = None;
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
                let decoded = construct(quote!(<#ty as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_unpack_at_depth(bytes, __depth)?))?;
                let missing = construct(quote!(<#ty as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_missing_optional()?))?;
                missing_optional = quote! { fn pssz_missing_optional() -> #k::pssz::PsszResult<Self> { ::core::result::Result::Ok(#missing) } };
                if unpack {
                    let inner = trusted_value(k, &quote!(__PsszFormat), ty, &bounds[0], quote!(bytes));
                    trusted_body = Some(attrs::construct(quote!(Self), &data.fields, &[inner])?);
                }
                validation_body = quote! { <#ty as #k::pssz::PsszValidate<__PsszFormat>>::pssz_validate_at_depth(bytes, __depth) };
                fixed_body = Some(quote! { #k::pssz::PsszPack::<__PsszFormat>::pssz_pack_fixed(#member_ref, slot, __depth) });
                size_body = Some(quote! { #k::pssz::PsszPack::<__PsszFormat>::pssz_size_at_depth(#member_ref, __depth) });
                size_fixed_body = Some(quote! { #k::pssz::PsszPack::<__PsszFormat>::pssz_size_fixed(#member_ref, __depth) });
                hint_body = Some(quote! { #k::pssz::PsszPack::<__PsszFormat>::pssz_size_hint(#member_ref) });
                (
                    if unpack {
                        quote! { ::core::result::Result::Ok(#decoded) }
                    } else {
                        quote! { #k::pssz::PsszPack::<__PsszFormat>::pssz_pack_at_depth(#member_ref, out, __depth) }
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
                            const INFERRED_BOUND: ::core::option::Option<usize> = <#ty as #pack>::INFERRED_BOUND;
                            const OFFSET_WIDTH: usize = <#ty as #pack>::OFFSET_WIDTH;
                            // A transparent type is its member, at the same position.
                            const RECORD_WIDTH_BENEATH: usize = <#ty as #pack>::RECORD_WIDTH_BENEATH;
                            const SHAPE: #k::pssz::Shape = <#ty as #pack>::SHAPE;
                        },
                    },
                )
            } else {
                let dwnc = opts.dwnc;
                let frame = self_frame(k, opts.cap);
                // The members lie beneath this record's cap, or beneath none.
                let m = &frame.members;
                let mpack = quote!(#k::pssz::PsszPack<#m>);
                let capped = opts.cap.is_some();
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
                let max_fields = match opts.max_fields {
                    Some(max) => { let max = max as usize; quote!(::core::option::Option::Some(#max)) }
                    None => quote!(::core::option::Option::None),
                };
                validation_body = record_validate(k, &data.fields, dwnc, &frame, true)?;
                if unpack {
                    trusted_body = Some(record_decode_trusted(k, &data.fields, quote!(Self), dwnc, &frame, true)?);
                } else {
                    fixed_body = Some(record_encode_fixed(k, &types, &values, &frame));
                    hint_body = Some(record_hint(k, &values, dwnc, &frame));
                    size_body = Some(record_size(k, &types, &values, dwnc, &frame));
                    size_fixed_body = Some(record_size_fixed(k, &values, &frame));
                }
                (
                    if unpack {
                        record_decode(k, &data.fields, quote!(Self), dwnc, &frame, true)?
                    } else {
                        record_encode(k, &types, &values, dwnc, &frame)
                    },
                    Consts {
                        fixed: quote!(#dwnc #(&& <#types as #mpack>::IS_FIXED_SIZE)*),
                        size: quote!(0usize #(.saturating_add(<#types as #mpack>::FIXED_SIZE))*),
                        // An extensible record always has its header; a DWNC
                        // record is as small as its fixed region.
                        min: if dwnc {
                            quote!(#k::pssz_checked::fixed_region(1, &[#(#k::pssz_checked::Layout::of::<#m, #types>()),*]))
                        } else {
                            quote!(1)
                        },
                        extra: quote! {
                            const MAX_DYNAMIC_DATA: ::core::option::Option<usize> = #cap;
                            // The bound inferred from the members (C++
                            // `inferred_bound<T>()`, docs/pssz-spec.md
                            // 3.2): a cap is not part of it, and a capped
                            // member counts as its cap.
                            const INFERRED_BOUND: ::core::option::Option<usize> = #k::pssz_checked::record_bound(&[
                                #((<#types as #mpack>::AUTO_BOUND, <#types as #mpack>::IS_FIXED_SIZE)),*
                            ], #dwnc);
                            // The largest encoding (C++
                            // `max_encoded_size<T>()`, docs/pssz-spec.md
                            // 3.7): the header and slots at the record's
                            // real width, the members at their own
                            // maximum, never more than the cap.
                            const MAX_ENCODED_SIZE: ::core::option::Option<usize> = #k::pssz_checked::record_max(&[
                                #((<#types as #mpack>::MAX_ENCODED_SIZE, <#types as #mpack>::IS_FIXED_SIZE)),*
                            ], #dwnc, <Self as #pack>::OFFSET_WIDTH, <Self as #pack>::MAX_DYNAMIC_DATA);
                            // A record keeps its own width wherever it appears.
                            const RECORD_WIDTH_BENEATH: usize = <Self as #pack>::OFFSET_WIDTH;
                            const CHILD_WIDER_THAN_CAP: bool = #capped
                                && #k::pssz_checked::widest(&[#(<#types as #mpack>::RECORD_WIDTH_BENEATH),*])
                                    > <Self as #pack>::OFFSET_WIDTH;
                            const SHAPE: #k::pssz::Shape = #k::pssz::Shape::record(
                                false, #dwnc, <Self as #pack>::OFFSET_WIDTH, <Self as #pack>::IS_FIXED_SIZE,
                                &[#(<#types as #mpack>::SHAPE),*])
                                .limits(<Self as #pack>::MAX_DYNAMIC_DATA, <Self as #pack>::INFERRED_BOUND, #max_fields);
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
                let __value = <#repr as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_unpack_at_depth(bytes, __depth)?;
                #(if __value == #values { return ::core::result::Result::Ok(()); })*
                #unknown_ok
            };
            if unpack {
                let unknown_trusted = match shape.other {
                    Some(other) => {
                        let other = &other.ident;
                        quote!(Self::#other(__value))
                    }
                    None => quote!(#k::pssz::trusted_bytes_invalid(#k::pssz::PsszError::new("pssz: value is not an enumerator"))),
                };
                trusted_body = Some(quote! {
                    #preamble
                    let __value = <#repr as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_unpack_trusted(bytes);
                    #(if __value == #values { return Self::#variants; })*
                    #unknown_trusted
                });
            }
            (
                if unpack {
                    quote! {
                        #preamble
                        let __value = <#repr as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_unpack_at_depth(bytes, __depth)?;
                        #(if __value == #values { return ::core::result::Result::Ok(Self::#variants); })*
                        #unknown_value
                    }
                } else {
                    fixed_body = Some(quote! {
                        #preamble
                        let __value: #repr = match self {
                            #(Self::#variants => #values,)*
                            #pack_other
                        };
                        <#repr as #pack>::pssz_pack_fixed(&__value, slot, __depth)
                    });
                    size_body = Some(quote! {
                        #preamble
                        let __value: #repr = match self {
                            #(Self::#variants => #values,)*
                            #pack_other
                        };
                        <#repr as #pack>::pssz_size_at_depth(&__value, __depth)
                    });
                    size_fixed_body = Some(quote! {
                        #preamble
                        let __value: #repr = match self {
                            #(Self::#variants => #values,)*
                            #pack_other
                        };
                        <#repr as #pack>::pssz_size_fixed(&__value, __depth)
                    });
                    quote! {
                        #preamble
                        let __value: #repr = match self {
                            #(Self::#variants => #values,)*
                            #pack_other
                        };
                        <#repr as #pack>::pssz_pack_at_depth(&__value, out, __depth)
                    }
                },
                Consts {
                    fixed: quote!(true),
                    size: quote!(::core::mem::size_of::<#repr>()),
                    min: quote!(::core::mem::size_of::<#repr>()),
                    extra: quote! {
                        const MAX_ENCODED_SIZE: ::core::option::Option<usize> = ::core::option::Option::Some(::core::mem::size_of::<#repr>());
                        const INFERRED_BOUND: ::core::option::Option<usize> = ::core::option::Option::Some(::core::mem::size_of::<#repr>());
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
            let mut trusted_arms = Vec::new();
            let mut size_arms = Vec::new();
            let mut hint_arms = Vec::new();
            let mut validate_arms = Vec::new();
            let mut shapes = Vec::new();
            let mut bounds = Vec::new();
            let mut maxima = Vec::new();
            let mut records = Vec::new();
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
                // Several unnamed fields are a tuple at the variant's
                // position; named fields are the members of a record
                // without a cap.
                let named = matches!(&variant.fields, Fields::Named(_));
                let frame = tuple_frame(k, &types, if named { Position::Free } else { Position::Same });
                let m = &frame.members;
                let tpack = quote!(#k::pssz::PsszPack<#m>);
                // What the alternative reserves: nothing, its one
                // payload, or the tuple of its fields.
                bounds.push(match &variant.fields {
                    Fields::Unit => quote!(::core::option::Option::Some(0usize)),
                    _ if single => { let ty = types[0]; quote!(<#ty as #pack>::AUTO_BOUND) }
                    _ => quote!(<(#(#types,)*) as #tpack>::AUTO_BOUND),
                });
                maxima.push(match &variant.fields {
                    Fields::Unit => quote!(::core::option::Option::Some(0usize)),
                    _ if single => { let ty = types[0]; quote!(<#ty as #pack>::MAX_ENCODED_SIZE) }
                    _ => quote!(<(#(#types,)*) as #tpack>::MAX_ENCODED_SIZE),
                });
                shapes.push(match &variant.fields {
                    Fields::Unit => quote!(#k::pssz::Shape::record(false, true, #k::pssz::offset_width::<__PsszFormat>(::core::option::Option::Some(0)), true, &[])),
                    _ if single => { let ty = types[0]; quote!(<#ty as #pack>::SHAPE) }
                    _ => quote!(<(#(#types,)*) as #tpack>::SHAPE),
                });
                // The widest record at the alternative's position.
                records.push(match &variant.fields {
                    Fields::Unit => quote!(0usize),
                    _ if single => { let ty = types[0]; quote!(<#ty as #pack>::RECORD_WIDTH_BENEATH) }
                    _ if named => quote!(<(#(#types,)*) as #tpack>::OFFSET_WIDTH),
                    _ => quote!(<(#(#types,)*) as #tpack>::RECORD_WIDTH_BENEATH),
                });
                validate_arms.push(match &variant.fields {
                    Fields::Unit => quote! { #tag => { #k::pssz_checked::decode_node(__depth + 1)?; if bytes.is_empty() { ::core::result::Result::Ok(()) } else { ::core::result::Result::Err(#k::pssz::PsszError::new("pssz: zero-size fixed payload must be empty")) } } },
                    _ if single => { let ty = types[0]; quote! { #tag => { <#ty as #k::pssz::PsszValidate<__PsszFormat>>::pssz_validate_at_depth(bytes, __depth + 1) } } }
                    _ => { let body = record_validate(k, &variant.fields, false, &frame, false)?; quote! { #tag => { #k::pssz_checked::decode_node(__depth + 1)?; let __depth = __depth + 1; #body } } }
                });
                if unpack {
                    let decode = match &variant.fields {
                        Fields::Unit => {
                            quote! { #k::pssz_checked::decode_node(__depth + 1)?; if !bytes.is_empty() { return ::core::result::Result::Err(#k::pssz::PsszError::new("pssz: zero-size fixed payload must be empty")); } ::core::result::Result::Ok(Self::#vname) }
                        }
                        _ if single => {
                            let ty = types[0];
                            let value = decode_value(k, &variant_bounds[0], quote!(<#ty as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_unpack_at_depth(bytes, __depth + 1)?));
                            quote! { ::core::result::Result::Ok(Self::#vname(#value)) }
                        }
                        _ => {
                            let body = record_decode(k, &variant.fields, quote!(Self::#vname), false, &frame, false)?;
                            quote! { #k::pssz_checked::decode_node(__depth + 1)?; let __depth = __depth + 1; #body }
                        }
                    };
                    arms.push(quote!(#tag => { #decode }));
                    let trusted = match &variant.fields {
                        Fields::Unit => quote!(Self::#vname),
                        _ if single => {
                            let value = trusted_value(k, &quote!(__PsszFormat), types[0], &variant_bounds[0], quote!(bytes));
                            quote!(Self::#vname(#value))
                        }
                        _ => record_decode_trusted(k, &variant.fields, quote!(Self::#vname), false, &frame, false)?,
                    };
                    trusted_arms.push(quote!(#tag => { #trusted }));
                } else {
                    let pattern = match &variant.fields {
                        Fields::Unit => quote!(Self::#vname),
                        Fields::Unnamed(_) => quote!(Self::#vname(#(#bindings),*)),
                        Fields::Named(_) => {
                            let members: Vec<_> = info.iter().map(|(m, _)| m).collect();
                            quote!(Self::#vname { #(#members: #bindings),* })
                        }
                    };
                    // The payload is the node beneath the variant's.
                    let (encode, hint, size) = match &variant.fields {
                        Fields::Unit => (
                            quote! { #k::pssz_checked::encode_node(__depth + 1)?; ::core::result::Result::Ok(()) },
                            quote!(1usize),
                            quote! { #k::pssz_checked::encode_node(__depth + 1)?; ::core::result::Result::Ok(1usize) },
                        ),
                        _ if single => {
                            let b = &bindings[0];
                            let value = encode_value(k, &variant_bounds[0], quote!(#b));
                            (
                                quote!(#k::pssz::PsszPack::<__PsszFormat>::pssz_pack_at_depth(#value, out, __depth + 1)),
                                quote!(#k::pssz::PsszPack::<__PsszFormat>::pssz_size_hint(#value).saturating_add(1)),
                                quote! {
                                    ::core::result::Result::Ok(
                                        #k::pssz::PsszPack::<__PsszFormat>::pssz_size_at_depth(#value, __depth + 1)?.saturating_add(1))
                                },
                            )
                        }
                        _ => {
                            let values = bindings
                                .iter()
                                .zip(&variant_bounds)
                                .map(|(b, bound)| encode_value(k, bound, quote!(#b)))
                                .collect::<Vec<_>>();
                            let body = record_encode(k, &types, &values, false, &frame);
                            let hint = record_hint(k, &values, false, &frame);
                            let size = record_size(k, &types, &values, false, &frame);
                            (
                                quote! {
                                    #k::pssz_checked::encode_node(__depth + 1)?;
                                    let __depth = __depth + 1;
                                    #body
                                },
                                quote!(#hint.saturating_add(1)),
                                quote! {
                                    #k::pssz_checked::encode_node(__depth + 1)?;
                                    let __depth = __depth + 1;
                                    let __payload: #k::pssz::PsszResult<usize> = { #size };
                                    ::core::result::Result::Ok(__payload?.saturating_add(1))
                                },
                            )
                        }
                    };
                    arms.push(quote!(#pattern => { #k::pssz_checked::append(out, &[#tag])?; #encode }));
                    hint_arms.push(quote!(#pattern => { #hint }));
                    size_arms.push(quote!(#pattern => { #size }));
                }
            }
            validation_body = quote! {
                let (&selector, bytes) = bytes.split_first().ok_or(#k::pssz::PsszError::new("pssz: variant span out of bounds"))?;
                match selector { #(#validate_arms,)* _ => ::core::result::Result::Err(#k::pssz::PsszError::new("pssz: variant selector out of range")) }
            };
            let body = if unpack {
                trusted_body = Some(quote! {
                    let (&selector, bytes) = match bytes.split_first() {
                        ::core::option::Option::Some(split) => split,
                        ::core::option::Option::None => #k::pssz::trusted_bytes_invalid(#k::pssz::PsszError::new("pssz: variant span out of bounds")),
                    };
                    match selector {
                        #(#trusted_arms,)*
                        _ => #k::pssz::trusted_bytes_invalid(#k::pssz::PsszError::new("pssz: variant selector out of range")),
                    }
                });
                quote! {
                    let (&selector, bytes) = bytes.split_first().ok_or(#k::pssz::PsszError::new("pssz: variant span out of bounds"))?;
                    match selector { #(#arms,)* _ => ::core::result::Result::Err(#k::pssz::PsszError::new("pssz: variant selector out of range")) }
                }
            } else {
                hint_body = Some(quote! { match self { #(#hint_arms),* } });
                size_body = Some(quote! { match self { #(#size_arms),* } });
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
                        // (C++ inferred_bound / max_encoded_size of a
                        // std::variant).
                        const INFERRED_BOUND: ::core::option::Option<usize> =
                            #k::pssz::variant_bound(&[#(#bounds),*]);
                        const MAX_ENCODED_SIZE: ::core::option::Option<usize> =
                            #k::pssz::variant_bound(&[#(#maxima),*]);
                        const RECORD_WIDTH_BENEATH: usize = #k::pssz_checked::widest(&[#(#records),*]);
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
        quote!(#k::pssz_checked::decode_node(__depth)?;)
    };
    let encode_depth_guard = if forwards {
        quote!()
    } else {
        quote!(#k::pssz_checked::encode_node(__depth)?;)
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
    let pack_fixed = match &fixed_body {
        Some(fixed) => quote! {
            #[allow(unused_variables)]
            fn pssz_pack_fixed(&self, slot: &mut [u8], __depth: usize) -> #k::pssz::PsszResult<()> {
                if !<Self as #k::pssz::PsszPack<__PsszFormat>>::IS_FIXED_SIZE
                    || slot.len() != <Self as #k::pssz::PsszPack<__PsszFormat>>::FIXED_SIZE
                {
                    return #k::pssz_checked::pack_fixed_by_append::<__PsszFormat, Self>(self, slot, __depth);
                }
                #encode_depth_guard
                let result: #k::pssz::PsszResult<()> = (|| { #fixed })();
                result?;
                #pack_check
                ::core::result::Result::Ok(())
            }
        },
        None => quote!(),
    };
    // A fixed-size record is its slot, appended: the fields land at
    // constant positions of one slice.
    let append_fixed = if fixed_body.is_some() && !forwards {
        quote! {
            if <Self as #k::pssz::PsszPack<__PsszFormat>>::IS_FIXED_SIZE {
                let __size = <Self as #k::pssz::PsszPack<__PsszFormat>>::FIXED_SIZE;
                let __start = out.len();
                #k::pssz_checked::append_zeros(out, __size)?;
                return <Self as #k::pssz::PsszPack<__PsszFormat>>::pssz_pack_fixed(self, &mut out[__start..], __depth);
            }
        }
    } else {
        quote!()
    };
    // The size passes: `pssz_size_at_depth` is `pssz_pack_at_depth` and
    // `pssz_size_fixed` is `pssz_pack_fixed`, statement by statement,
    // without the output.
    let size_fixed = match &size_fixed_body {
        Some(fixed) => quote! {
            #[allow(unused_variables)]
            fn pssz_size_fixed(&self, __depth: usize) -> #k::pssz::PsszResult<()> {
                if !<Self as #k::pssz::PsszPack<__PsszFormat>>::IS_FIXED_SIZE {
                    return #k::pssz_checked::size_fixed_by_size::<__PsszFormat, Self>(self, __depth);
                }
                #encode_depth_guard
                let result: #k::pssz::PsszResult<()> = (|| { #fixed })();
                result?;
                #pack_check
                ::core::result::Result::Ok(())
            }
        },
        None => quote!(),
    };
    let size_append_fixed = if size_fixed_body.is_some() && !forwards {
        quote! {
            if <Self as #k::pssz::PsszPack<__PsszFormat>>::IS_FIXED_SIZE {
                <Self as #k::pssz::PsszPack<__PsszFormat>>::pssz_size_fixed(self, __depth)?;
                return ::core::result::Result::Ok(<Self as #k::pssz::PsszPack<__PsszFormat>>::FIXED_SIZE);
            }
        }
    } else {
        quote!()
    };
    let size_at_depth = match &size_body {
        Some(size) => quote! {
            #[allow(unused_variables)]
            fn pssz_size_at_depth(&self, __depth: usize) -> #k::pssz::PsszResult<usize> {
                #size_append_fixed
                #encode_depth_guard
                let result: #k::pssz::PsszResult<usize> = (|| { #size })();
                let __size = result?;
                #pack_check
                ::core::result::Result::Ok(__size)
            }
        },
        None => quote!(),
    };
    let size_hint = match &hint_body {
        Some(hint) => quote! {
            #[allow(unused_variables)]
            fn pssz_size_hint(&self) -> usize { #hint }
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
                    fn pssz_validate_at_depth(bytes: &[u8], __depth: usize) -> #k::pssz::PsszResult<()> {
                        <Self as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_unpack_at_depth(bytes, __depth).map(|_| ())
                    }
                }
            }
        } else {
            quote! {
                impl #validation_g #k::pssz::PsszValidate<__PsszFormat> for #name #type_g #validation_where {
                    #[inline]
                    fn pssz_validate(bytes: &[u8]) -> #k::pssz::PsszResult<()> {
                        <Self as #k::pssz::PsszValidate<__PsszFormat>>::pssz_validate_at_depth(bytes, #k::pssz_checked::decode_depth())
                    }
                    // The depth of the node is an argument, not a
                    // thread-local counter.
                    #[allow(unused_variables)]
                    fn pssz_validate_at_depth(bytes: &[u8], __depth: usize) -> #k::pssz::PsszResult<()> {
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
                #[inline]
                fn pssz_unpack(bytes: &[u8]) -> #k::pssz::PsszResult<Self> {
                    <Self as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_unpack_at_depth(bytes, #k::pssz_checked::decode_depth())
                }
                // The depth of the node is an argument, not a thread-local
                // counter.
                #[allow(unused_variables)]
                fn pssz_unpack_at_depth(bytes: &[u8], __depth: usize) -> #k::pssz::PsszResult<Self> {
                    #depth_guard
                    let __value: Self = (|| -> #k::pssz::PsszResult<Self> { #body })()?;
                    #unpack_check
                    ::core::result::Result::Ok(__value)
                }
                fn pssz_unpack_boxed_at_depth(bytes: &[u8], __depth: usize) -> #k::pssz::PsszResult<::std::boxed::Box<Self>> {
                    ::core::result::Result::Ok(::std::boxed::Box::new(
                        <Self as #k::pssz::PsszUnpack<__PsszFormat>>::pssz_unpack_at_depth(bytes, __depth)?))
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
                    <Self as #k::pssz::PsszPack<__PsszFormat>>::pssz_size_at_depth(self, #k::pssz_checked::encode_depth())
                        .expect("pssz: encode failed; use try_to_pssz")
                }
                fn pssz_pack(&self, out: &mut ::std::vec::Vec<u8>) { <Self as #k::pssz::PsszPack<__PsszFormat>>::pssz_pack_checked(self, out).expect("pssz: encode failed; use try_to_pssz"); }
                fn pssz_pack_checked(&self, out: &mut ::std::vec::Vec<u8>) -> #k::pssz::PsszResult<()> {
                    #k::pssz_checked::pack_checked::<__PsszFormat, Self>(self, out)
                }
                // The value is written once, where it belongs in `out`; on
                // error the caller that owns the transaction truncates.
                #[allow(unused_variables)]
                fn pssz_pack_at_depth(&self, out: &mut ::std::vec::Vec<u8>, __depth: usize) -> #k::pssz::PsszResult<()> {
                    #append_fixed
                    #encode_depth_guard
                    let result: #k::pssz::PsszResult<()> = (|| { #body })();
                    result?;
                    #pack_check
                    ::core::result::Result::Ok(())
                }
                #pack_fixed
                #size_at_depth
                #size_fixed
                #size_hint
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

/// What a record carries besides its members: the fixed layout, the cap, the
/// field-count cap and, for a method's argument list, whether its
/// parameters have names (`tuple`).
#[derive(Clone, Copy, Default)]
struct RecordInfo {
    dwnc: bool,
    cap: Option<u64>,
    max_fields: Option<u64>,
    arguments: Option<bool>,
}

fn schema_record(k: &syn::Path, fields: &Fields, info: RecordInfo, forms: Forms, case: Option<Case>) -> syn::Result<Tokens> {
    let RecordInfo { dwnc, cap, max_fields, arguments } = info;
    let members = attrs::members(fields)?;
    // The member's name in text: its identifier, `#[psio(name)]` /
    // `#[serde(rename)]`, or the identifier under the `rename_all` convention.
    // The members of an argument list without parameter names are named by
    // position.
    let names: Vec<_> = members
        .iter()
        .enumerate()
        .map(|(position, m)| match &m.member {
            _ if arguments == Some(false) => position.to_string(),
            Member::Named(_) => attrs::member_name(m.field, &m.attrs, case),
            Member::Unnamed(index) => index.index.to_string(),
        })
        .collect();
    for (i, name) in names.iter().enumerate() {
        if names[..i].contains(name) {
            return Err(syn::Error::new_spanned(members[i].field, "duplicate member name"));
        }
    }
    // A member with bounds is described as the type its annotation says:
    // that type states the attributes with no type form (`lengthBound`).
    let types: Vec<_> = field_info(k, fields)?.into_iter().map(|(_, ty)| ty).collect();
    // The member's node, as C++ SchemaBuilder writes it. A record member:
    // its plain type is interned, and under an annotation the member is the
    // annotated type, inline (`build_record_member`). A parameter of an
    // argument list: the annotated type alone (`psio_annotated_tuple`).
    let nodes: Vec<_> = members
        .iter()
        .map(|m| {
            let ty = &m.field.ty;
            Ok(match attrs::annotated(m.field, &m.attrs, k)? {
                Some(attrs::Annotated { own, spec }) if arguments.is_some() => {
                    quote!(<#own as #k::pssz::bounds::SchemaAs<#spec>>::annotated(builder)?)
                }
                Some(attrs::Annotated { own, spec }) => {
                    quote!(<#own as #k::pssz::bounds::SchemaAs<#spec>>::member(builder)?)
                }
                None => quote!(<#ty as #k::pssz::schema_ir::ToPsszSchema>::schema(builder)?),
            })
        })
        .collect::<syn::Result<Vec<_>>>()?;
    let member_attributes = members
        .iter()
        .map(|m| schema_int_forms(k, m.field, forms))
        .collect::<syn::Result<Vec<_>>>()?;
    // What the numbered formats read of a record member, after the others
    // and in the order C++ `build_record_member` writes them: its stated
    // field number, that it is retired, and a scalar default that is not
    // zero. (A parameter of an argument list has none of them.)
    let numbered: Vec<Tokens> = members
        .iter()
        .map(|m| {
            if arguments.is_some() {
                return quote!();
            }
            let mut out = Vec::new();
            if let Some(number) = m.attrs.field_number {
                out.push(schema_attribute(k, "field", &number.to_string()));
            }
            if m.attrs.deprecated {
                out.push(schema_attribute(k, "deprecated", ""));
            }
            let default = m.attrs.default.as_ref().map(|value| {
                let ty = &m.field.ty;
                quote! {
                    let __default: #ty = #value;
                    __attributes.extend(#k::pssz::schema_ir::default_attribute(&__default)?);
                }
            });
            quote!(#(__attributes.push(#out);)* #default)
        })
        .collect();
    // The order C++ SchemaBuilder emits.
    let mut attributes = Vec::new();
    if let Some(named) = arguments {
        attributes.push(schema_attribute(k, "tuple", if named { "named" } else { "positional" }));
    }
    if dwnc {
        attributes.push(schema_attribute(k, "definitionWillNotChange", ""));
    }
    if let Some(max_fields) = max_fields {
        attributes.push(schema_attribute(k, "maxFields", &max_fields.to_string()));
    }
    if let Some(cap) = cap {
        attributes.push(schema_attribute(k, "maxDynamicData", &cap.to_string()));
    }
    Ok(quote! {
        #k::pssz::schema_ir::AnyType::new(#k::pssz::schema_ir::AnyValue::Object(#k::pssz::schema_ir::Object {
            members: ::std::vec![#(#k::pssz::schema_ir::Member {
                name: #names.into(), r#type: ::std::boxed::Box::new(#nodes),
                attributes: {
                    #[allow(unused_mut)]
                    let mut __attributes: ::std::vec::Vec<#k::pssz::schema_ir::Attribute> = ::std::vec![#(#member_attributes),*];
                    __attributes.extend(<#types as #k::pssz::schema_ir::ToPsszSchema>::member_attributes());
                    #numbered
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
    for (ty, _) in generic_fields(&input, &opts) {
        generics
            .make_where_clause()
            .predicates
            .push(parse_quote!(#ty: #k::pssz::schema_ir::ToPsszSchema));
    }
    let (impl_g, _, where_c) = generics.split_for_impl();
    let (_, type_g, _) = input.generics.split_for_impl();
    let c_enum = c_like(&input, &opts)?;
    // An enum over an integer is a scalar: as a member's default it is the
    // decimal text of its value (nothing for zero).
    let default_text = c_enum.as_ref().filter(|shape| shape.repr.is_some()).map(|shape| {
        let (preamble, values) = shape.values();
        let idents = shape.declared.iter().map(|v| &v.ident);
        let other = shape.other.map(|v| {
            let ident = &v.ident;
            quote!(Self::#ident(value) => *value,)
        });
        quote! {
            fn default_text(&self) -> ::core::option::Option<::std::string::String> {
                #preamble
                let value = match self {
                    #(Self::#idents => #values,)*
                    #other
                };
                ::core::option::Option::Some(if value == 0 { ::std::string::String::new() } else { value.to_string() })
            }
        }
    });
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
            schema_record(
                k,
                &data.fields,
                RecordInfo { dwnc: opts.dwnc, cap: opts.cap, max_fields: opts.max_fields, arguments: opts.arguments },
                opts.forms,
                opts.rename_all,
            )?
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
            let mut labels = ::std::collections::BTreeSet::new();
            for variant in &data.variants {
                if variant.discriminant.is_some() {
                    return Err(syn::Error::new_spanned(
                        variant,
                        "PSSZ selectors use declaration order",
                    ));
                }
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
                        RecordInfo::default(),
                        Forms::default(),
                        variant_attrs.rename_all.or(opts.rename_all_fields),
                    )?,
                };
                // The alternative's label — its one name, in the schema and
                // as the key of the tagged text form: the variant's
                // identifier, or what `#[psio(name)]` / `#[serde(rename)]` /
                // the enum's `rename_all` makes of it.
                let label = attrs::variant_name(variant, &variant_attrs, opts.rename_all);
                if label.is_empty() || !labels.insert(label.clone()) {
                    return Err(syn::Error::new_spanned(
                        variant,
                        format!("the alternatives of a variant have distinct, nonempty labels: \"{label}\" is given twice or is empty"),
                    ));
                }
                members.push(quote! { #k::pssz::schema_ir::Member { name: #label.into(), r#type: ::std::boxed::Box::new(#ty), attributes: ::std::vec![] } });
            }
            // An untagged variant: in text the bare value of the alternative.
            let untagged: Vec<_> =
                opts.untagged.then(|| schema_attribute(k, "untagged", "")).into_iter().collect();
            quote! { #k::pssz::schema_ir::AnyType::new(#k::pssz::schema_ir::AnyValue::Variant(#k::pssz::schema_ir::Variant { members: ::std::vec![#(#members),*], attributes: ::std::vec![#(#untagged),*] })) }
        }
        Data::Union(data) => {
            return Err(syn::Error::new_spanned(
                data.union_token,
                "PSSZ schema does not support Rust unions",
            ))
        }
    };
    // A lifetime is not part of a schema: `S<'a>` is named `S`, as its
    // owned form is. A type or const parameter makes a family of types,
    // each named by its full type name.
    let type_generic = input.generics.params.iter().any(|param| !matches!(param, syn::GenericParam::Lifetime(_)));
    let schema_name = if opts.arguments.is_some() {
        // A method's argument list has no name of its own (C++: the
        // argument tuple, an `@N` node); a schema names it with
        // `SchemaBuilder::insert_named`.
        quote!(::core::option::Option::None)
    } else if let Some(name) = &opts.name {
        quote!(::core::option::Option::Some(#name.into()))
    } else if !type_generic {
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
        if !type_generic && attrs::reserved_custom_id(&implied) {
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
            #default_text
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
    for (ty, m) in generic_fields(&input, &opts) {
        generics
            .make_where_clause()
            .predicates
            .push(parse_quote!(#ty: #k::pssz::PsszPack<#m>));
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
            for (ty, m) in generic_fields(&input, &opts) {
                field_generics
                    .make_where_clause()
                    .predicates
                    .push(parse_quote!(#ty: #k::pssz::PsszPack<#m>));
            }
            // The format of the members' position: a view of a member of a
            // capped record reads what lies beneath the cap at its widths.
            let m = positions(&input, &opts)[0].format(k);
            let (field_g, _, field_where) = field_generics.split_for_impl();
            for (index, (member, ty)) in info.iter().enumerate() {
                let method = match member {
                    Member::Named(id) => id.clone(),
                    Member::Unnamed(index) => format_ident!("field_{}", index.index),
                };
                let try_method = format_ident!("try_{}", method.to_string().trim_start_matches("r#"));
                declarations.push(quote!(fn #method(&self) -> #k::pssz_view::PsszView<'__pssz_view, #ty, #m>;));
                declarations.push(quote!(
                    /// Fallible form for unvalidated buffers.
                    fn #try_method(&self) -> #k::pssz::PsszResult<#k::pssz_view::PsszView<'__pssz_view, #ty, #m>>;
                ));
                methods.push(quote! { fn #method(&self) -> #k::pssz_view::PsszView<'__pssz_view, #ty, #m> { self.field::<#index>() } });
                methods.push(quote! { fn #try_method(&self) -> #k::pssz::PsszResult<#k::pssz_view::PsszView<'__pssz_view, #ty, #m>> { self.try_field::<#index>() } });
                let bare = method.to_string().trim_start_matches("r#").to_string();
                let mut_method = format_ident!("{}_mut", bare);
                let try_mut_method = format_ident!("try_{}_mut", bare);
                mut_declarations.push(quote!(
                    /// The field for writing; panics when it is not stored in the frame.
                    fn #mut_method(&mut self) -> #k::pssz_view::PsszMutView<'_, #ty, #m>;
                    fn #try_mut_method(&mut self) -> #k::pssz::PsszResult<#k::pssz_view::PsszMutView<'_, #ty, #m>>;
                ));
                mut_methods.push(quote! {
                    fn #mut_method(&mut self) -> #k::pssz_view::PsszMutView<'_, #ty, #m> { self.field_mut::<#index>() }
                    fn #try_mut_method(&mut self) -> #k::pssz::PsszResult<#k::pssz_view::PsszMutView<'_, #ty, #m>> { self.try_field_mut::<#index>() }
                });
                let dwnc = opts.dwnc;
                let transparent = opts.transparent;
                field_impls.push(quote! {
                    impl #field_g #k::pssz_view::PsszField<__PsszFormat, #index> for #name #type_g #field_where {
                        type Beneath = #m;
                        type Field = #ty;
                        const LOCATION: #k::pssz_checked::FieldLocation = #k::pssz_checked::record_location(
                            <Self as #k::pssz::PsszPack<__PsszFormat>>::OFFSET_WIDTH, &[
                            #(#k::pssz_checked::Layout::of::<#m, #types>()),*
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
                // The fields of a struct-like variant are a record's members.
                let m = if matches!(&variant.fields, Fields::Named(_)) { Position::Free } else { Position::Same }.format(k);
                declarations.push(quote!(fn #method(&self) -> ::core::option::Option<#k::pssz_view::PsszView<'__pssz_view, #payload, #m>>;));
                methods.push(quote! {
                    fn #method(&self) -> ::core::option::Option<#k::pssz_view::PsszView<'__pssz_view, #payload, #m>> {
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
