//! The Protocol Buffers derive: `Protobuf` (psio `protobuf`, C++
//! `psio/protobuf.hpp`).
//!
//! * struct → message. A member's number is `#[psio(field = N)]`, the
//!   1-based declaration position by default; a oneof member takes one
//!   number per alternative, counting up from its own. The numbers are
//!   checked at compile time. Members are written in ascending number
//!   order. `#[psio(pb_sint)]` / `#[psio(pb_fixed)]` choose how the
//!   member's integers are written.
//! * field-less `#[repr(int)]` enum (of 32 bits or fewer) → a protobuf enum;
//!   a `#[psio(other)]` variant holds the values that have no name.
//! * enum whose variants hold one value each → a oneof (a member of a
//!   message); a unit first variant is "none set", otherwise an absent
//!   oneof reads as the first variant at its zero value.
//!
//! What a member type maps to is decided by its `PbField` / `PbSingle`
//! implementation, which a derive cannot see; the code written here asks it.

use proc_macro::TokenStream;
use proc_macro2::TokenStream as Tokens;
use quote::quote;
use syn::{parse_macro_input, Data, DeriveInput, Fields};

use crate::attrs::{self, Derive};
use crate::c_enum::{self, CEnum};

pub fn protobuf_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    expand(&input).unwrap_or_else(|e| e.to_compile_error()).into()
}

fn expand(input: &DeriveInput) -> syn::Result<Tokens> {
    let item = attrs::item(input, Derive::Interchange("Protobuf"))?;
    let k = item.krate();
    if item.transparent {
        return Err(syn::Error::new_spanned(
            &input.ident,
            "derive(Protobuf) has no transparent types: a message is a record of numbered fields",
        ));
    }
    if !input.generics.params.is_empty() {
        return Err(syn::Error::new_spanned(
            &input.generics,
            "derive(Protobuf) does not support generic parameters",
        ));
    }
    match &input.data {
        Data::Struct(data) => message(input, &data.fields, item.invariant.as_ref(), &k),
        Data::Enum(_) => match c_enum::analyze(input)? {
            Some(shape) => enumeration(input, &shape, &k),
            None => oneof(input, &k),
        },
        Data::Union(_) => Err(syn::Error::new_spanned(&input.ident, "derive(Protobuf) applies to structs and enums")),
    }
}

fn message(input: &DeriveInput, fields: &Fields, invariant: Option<&syn::Path>, k: &syn::Path) -> syn::Result<Tokens> {
    let name = &input.ident;
    let pb = quote!(#k::protobuf);
    let members = attrs::members(fields)?;

    struct Slot {
        number: u32,
        member: syn::Member,
        ty: syn::Type,
        enc: Tokens,
        needs: u8, // 0: nothing; 1: an integer; 2: a signed integer
    }
    let mut slots: Vec<Slot> = members
        .iter()
        .enumerate()
        .map(|(index, m)| Slot {
            // C++ `field_number_v`: the 1-based position.
            number: m.attrs.field_number.unwrap_or(index as u32 + 1),
            member: m.member.clone(),
            ty: m.field.ty.clone(),
            enc: if m.attrs.pb_fixed {
                quote!(#pb::PbEnc::Fixed)
            } else if m.attrs.pb_sint {
                quote!(#pb::PbEnc::Zigzag)
            } else {
                quote!(#pb::PbEnc::Plain)
            },
            needs: if m.attrs.pb_sint {
                2
            } else if m.attrs.pb_fixed {
                1
            } else {
                0
            },
        })
        .collect();
    let zeros: Vec<Tokens> = members
        .iter()
        .map(|m| {
            let ty = &m.field.ty;
            quote!(<#ty as #pb::PbField>::pb_new())
        })
        .collect();
    let zero = attrs::construct(quote!(#name), fields, &zeros)?;

    // Ascending numbers: the order on the wire. (The ranges of valid
    // members do not overlap, so the order of their first numbers is the
    // order of all of them; the assertions below refuse the rest.)
    slots.sort_by_key(|s| s.number);

    let mut checks = Vec::new();
    for (i, s) in slots.iter().enumerate() {
        let (number, ty) = (s.number, &s.ty);
        checks.push(quote! {
            assert!(#pb::pb_numbers_valid(#number, <#ty as #pb::PbField>::SLOTS),
                    "protobuf: a field number is within 1..536870911 and outside 19000..19999");
        });
        if let Some(next) = slots.get(i + 1) {
            let after = next.number;
            checks.push(quote! {
                assert!(#number as u64 + <#ty as #pb::PbField>::SLOTS as u64 <= #after as u64,
                        "protobuf: a field number is used twice (a oneof member takes one number per \
                         alternative, counting up from its own: give the members after it #[psio(field = N)])");
            });
        }
        match s.needs {
            1 => checks.push(quote! {
                assert!(<#ty as #pb::PbField>::INTEGER >= 1, "protobuf: pb_fixed is for integers");
            }),
            2 => checks.push(quote! {
                assert!(<#ty as #pb::PbField>::INTEGER == 2, "protobuf: pb_sint is for signed integers");
            }),
            _ => {}
        }
    }

    let sizes = slots.iter().map(|s| {
        let (number, member, ty, enc) = (s.number, &s.member, &s.ty, &s.enc);
        quote!(<#ty as #pb::PbField>::pb_size(&self.#member, #number, #enc))
    });
    let writes = slots.iter().rev().map(|s| {
        let (number, member, ty, enc) = (s.number, &s.member, &s.ty, &s.enc);
        quote!(<#ty as #pb::PbField>::pb_write(&self.#member, #number, #enc, __w);)
    });
    let reads = slots.iter().map(|s| {
        let (number, member, ty, enc) = (s.number, &s.member, &s.ty, &s.enc);
        quote! {
            if __number >= #number && __number - #number < <#ty as #pb::PbField>::SLOTS {
                <#ty as #pb::PbField>::pb_read(
                    __r, __end, __number - #number, __wire, #enc,
                    match &mut __out { Some(__o) => Some(&mut __o.#member), None => None })?
            } else
        }
    });

    // What the message declares about its values (psio::contract): the
    // members in declaration order, what each holds before its own
    // declarations, the invariant last.
    let contract = quote!(#k::contract);
    let mut declares = Vec::new();
    let mut declared = Vec::new();
    let mut own = invariant.is_some();
    for m in &members {
        let (member, ty) = (&m.member, &m.field.ty);
        declares.push(quote!(__any |= <#ty as #pb::PbField>::pb_declares(__seen);));
        declared.push(quote!(<#ty as #pb::PbField>::pb_check_declared(&self.#member)?;));
        if let Some(attrs::Annotated { own: field, spec }) = attrs::annotated(m.field, &m.attrs, k)? {
            own = true;
            declared.push(quote! {
                #contract::member::<#field, #spec>(&self.#member, "protobuf: member length_bound violated")?;
            });
        }
    }
    let invariant = invariant.map(|path| {
        quote! {
            if let ::core::result::Result::Err(__why) = #path(self) {
                return ::core::result::Result::Err(#contract::invariant(__why, "protobuf: invariant violated"));
            }
        }
    });

    Ok(quote! {
        const _: () = {
            #(#checks)*
        };
        impl #pb::PbMessage for #name {
            fn pb_message_zero() -> Self {
                #zero
            }
            fn pb_body_size(&self) -> usize {
                0usize #(+ #sizes)*
            }
            #[allow(unused_variables)]
            fn pb_write_body(&self, __w: &mut #pb::PbWriter) {
                #(#writes)*
            }
            #[allow(unused_mut, unused_variables)]
            fn pb_read_body(
                __r: &mut #pb::PbReader,
                __end: usize,
                mut __out: ::core::option::Option<&mut Self>,
            ) -> ::core::result::Result<(), #pb::PbError> {
                while __r.more(__end) {
                    let (__number, __wire) = __r.field(__end)?;
                    let __read: bool = #(#reads)* { false };
                    if !__read {
                        __r.skip(__end, __number, __wire)?;
                    }
                }
                ::core::result::Result::Ok(())
            }
            fn pb_message_declares(__seen: &mut #contract::Seen) -> bool {
                if !__seen.enter::<Self>() {
                    return false;
                }
                let mut __any = #own;
                #(#declares)*
                __any
            }
            fn pb_has_declarations() -> bool {
                static __KEPT: #contract::Declares = #contract::Declares::new();
                __KEPT.get(|| <Self as #pb::PbMessage>::pb_message_declares(&mut #contract::Seen::new()))
            }
            fn pb_message_check_declared(&self) -> ::core::result::Result<(), &'static str> {
                #(#declared)*
                #invariant
                ::core::result::Result::Ok(())
            }
        }
        impl #pb::PbSingle for #name {
            fn pb_declares(__seen: &mut #contract::Seen) -> bool {
                <Self as #pb::PbMessage>::pb_message_declares(__seen)
            }
            fn pb_check_declared(&self) -> ::core::result::Result<(), &'static str> {
                <Self as #pb::PbMessage>::pb_message_check_declared(self)
            }
            fn pb_zero() -> Self {
                <Self as #pb::PbMessage>::pb_message_zero()
            }
            fn pb_absent(&self, _enc: #pb::PbEnc) -> bool {
                <Self as #pb::PbMessage>::pb_body_size(self) == 0
            }
            fn pb_payload_size(&self, _enc: #pb::PbEnc) -> usize {
                #pb::message_payload_size(self)
            }
            fn pb_write_payload(&self, _enc: #pb::PbEnc, __w: &mut #pb::PbWriter) {
                #pb::message_write_payload(self, __w)
            }
            fn pb_read_payload(
                __r: &mut #pb::PbReader,
                __end: usize,
                _enc: #pb::PbEnc,
                __out: ::core::option::Option<&mut Self>,
            ) -> ::core::result::Result<(), #pb::PbError> {
                #pb::message_read_payload(__r, __end, __out)
            }
        }
        impl #pb::PbField for #name {
            fn pb_declares(__seen: &mut #contract::Seen) -> bool {
                <Self as #pb::PbMessage>::pb_message_declares(__seen)
            }
            fn pb_check_declared(&self) -> ::core::result::Result<(), &'static str> {
                <Self as #pb::PbMessage>::pb_message_check_declared(self)
            }
            fn pb_new() -> Self {
                <Self as #pb::PbMessage>::pb_message_zero()
            }
            fn pb_size(&self, __number: u32, _enc: #pb::PbEnc) -> usize {
                #pb::message_field_size(self, __number)
            }
            fn pb_write(&self, __number: u32, _enc: #pb::PbEnc, __w: &mut #pb::PbWriter) {
                #pb::message_field_write(self, __number, __w)
            }
            fn pb_read(
                __r: &mut #pb::PbReader,
                __end: usize,
                _slot: u32,
                __wire: u8,
                __enc: #pb::PbEnc,
                __out: ::core::option::Option<&mut Self>,
            ) -> ::core::result::Result<bool, #pb::PbError> {
                #pb::single_read(__r, __end, __wire, __enc, __out)
            }
        }
    })
}

fn enumeration(input: &DeriveInput, shape: &CEnum, k: &syn::Path) -> syn::Result<Tokens> {
    let name = &input.ident;
    let pb = quote!(#k::protobuf);
    let Some(repr) = &shape.repr else {
        return Err(syn::Error::new_spanned(
            &input.ident,
            "a field-less enum is a protobuf enum of its underlying integer: add #[repr(u8)] \
             (or u16/u32/i8/i16/i32)",
        ));
    };
    if matches!(repr.to_string().as_str(), "u64" | "i64") {
        return Err(syn::Error::new_spanned(repr, "a protobuf enum is an int32: 32 bits or fewer"));
    }
    let (consts, values) = shape.values();
    let idents: Vec<_> = shape.declared.iter().map(|v| &v.ident).collect();
    let other_to = shape.other.map(|o| {
        let ident = &o.ident;
        quote!(Self::#ident(__n) => *__n,)
    });
    let other_from = match shape.other {
        Some(o) => {
            let ident = &o.ident;
            quote!(::core::option::Option::Some(Self::#ident(__n)))
        }
        None => quote!(::core::option::Option::None),
    };
    // The value a wire integer is, when the repr holds it.
    let narrow = if shape.signed() {
        quote!(<#repr as ::core::convert::TryFrom<i32>>::try_from(__bits as u32 as i32).ok())
    } else {
        quote!(<#repr as ::core::convert::TryFrom<u32>>::try_from(__bits as u32).ok())
    };
    let widen = if shape.signed() {
        quote!(__n as i32 as i64 as u64)
    } else {
        quote!(__n as u32 as i32 as i64 as u64)
    };
    // The zero value has to be one the enum holds.
    let zero_check = shape.other.is_none().then(|| {
        quote! {
            assert!(false #(|| #values == 0)*,
                    "protobuf: an absent enum reads as zero: declare a variant of value 0, or a #[psio(other)] variant");
        }
    });
    Ok(quote! {
        const _: () = {
            impl #name {
                #[doc(hidden)]
                fn __pb_value(&self) -> #repr {
                    #consts
                    match self {
                        #(Self::#idents => #values,)*
                        #other_to
                    }
                }
                #[doc(hidden)]
                fn __pb_from(__n: #repr) -> ::core::option::Option<Self> {
                    #consts
                    #(if __n == #values { return ::core::option::Option::Some(Self::#idents); })*
                    #other_from
                }
                #[doc(hidden)]
                const __PB_ZERO_CHECK: () = {
                    #consts
                    #zero_check
                };
            }
            impl #pb::PbSingle for #name {
                const SCALAR: bool = true;
                fn pb_wire(_enc: #pb::PbEnc) -> u8 {
                    #pb::WIRE_VARINT
                }
                fn pb_zero() -> Self {
                    let _ = Self::__PB_ZERO_CHECK;
                    Self::__pb_from(0).expect("checked at compile time")
                }
                fn pb_absent(&self, _enc: #pb::PbEnc) -> bool {
                    self.__pb_value() == 0
                }
                fn pb_payload_size(&self, _enc: #pb::PbEnc) -> usize {
                    let __n = self.__pb_value();
                    #pb::varint_size(#widen)
                }
                fn pb_write_payload(&self, _enc: #pb::PbEnc, __w: &mut #pb::PbWriter) {
                    let __n = self.__pb_value();
                    __w.varint(#widen)
                }
                fn pb_read_payload(
                    __r: &mut #pb::PbReader,
                    __end: usize,
                    _enc: #pb::PbEnc,
                    __out: ::core::option::Option<&mut Self>,
                ) -> ::core::result::Result<(), #pb::PbError> {
                    let __bits = __r.varint(__end)?;
                    let __value: ::core::option::Option<#repr> = #narrow;
                    match __value.and_then(Self::__pb_from) {
                        ::core::option::Option::Some(__v) => {
                            if let ::core::option::Option::Some(__o) = __out {
                                *__o = __v;
                            }
                            ::core::result::Result::Ok(())
                        }
                        ::core::option::Option::None => __r.fail("protobuf: a value does not fit its type"),
                    }
                }
            }
            impl #pb::PbField for #name {
                fn pb_new() -> Self {
                    <Self as #pb::PbSingle>::pb_zero()
                }
                fn pb_size(&self, __number: u32, __enc: #pb::PbEnc) -> usize {
                    #pb::single_size(self, __number, __enc)
                }
                fn pb_write(&self, __number: u32, __enc: #pb::PbEnc, __w: &mut #pb::PbWriter) {
                    #pb::single_write(self, __number, __enc, __w)
                }
                fn pb_read(
                    __r: &mut #pb::PbReader,
                    __end: usize,
                    _slot: u32,
                    __wire: u8,
                    __enc: #pb::PbEnc,
                    __out: ::core::option::Option<&mut Self>,
                ) -> ::core::result::Result<bool, #pb::PbError> {
                    #pb::single_read(__r, __end, __wire, __enc, __out)
                }
            }
        };
    })
}

fn oneof(input: &DeriveInput, k: &syn::Path) -> syn::Result<Tokens> {
    let name = &input.ident;
    let pb = quote!(#k::protobuf);
    let Data::Enum(data) = &input.data else { unreachable!() };
    let plain = quote!(#pb::PbEnc::Plain);

    // A unit first variant: none set (C++ std::monostate).
    let mut variants = data.variants.iter().peekable();
    let none = match variants.peek() {
        Some(v) if matches!(v.fields, Fields::Unit) => variants.next().map(|v| &v.ident),
        _ => None,
    };
    let mut alternatives = Vec::new();
    for v in variants {
        match &v.fields {
            Fields::Unnamed(f) if f.unnamed.len() == 1 => alternatives.push((&v.ident, &f.unnamed[0].ty)),
            _ => {
                return Err(syn::Error::new_spanned(
                    v,
                    "a oneof alternative holds exactly one unnamed value (a unit variant, first, is \"none set\")",
                ))
            }
        }
    }
    if alternatives.is_empty() {
        return Err(syn::Error::new_spanned(&input.ident, "a oneof has at least one alternative"));
    }
    let count = alternatives.len() as u32;
    let zero = match none {
        Some(ident) => quote!(Self::#ident),
        None => {
            let (ident, ty) = alternatives[0];
            quote!(Self::#ident(<#ty as #pb::PbSingle>::pb_zero()))
        }
    };
    let none_size = none.map(|ident| quote!(Self::#ident => 0,));
    let none_write = none.map(|ident| quote!(Self::#ident => {}));
    let index: Vec<u32> = (0..count).collect();
    let idents: Vec<_> = alternatives.iter().map(|a| a.0).collect();
    let types: Vec<_> = alternatives.iter().map(|a| a.1).collect();
    Ok(quote! {
        impl #pb::PbField for #name {
            const SLOTS: u32 = #count;
            fn pb_declares(__seen: &mut #k::contract::Seen) -> bool {
                if !__seen.enter::<Self>() {
                    return false;
                }
                let mut __any = false;
                #(__any |= <#types as #pb::PbSingle>::pb_declares(__seen);)*
                __any
            }
            #[allow(unreachable_patterns)]
            fn pb_check_declared(&self) -> ::core::result::Result<(), &'static str> {
                match self {
                    #(Self::#idents(__v) => <#types as #pb::PbSingle>::pb_check_declared(__v),)*
                    _ => ::core::result::Result::Ok(()),
                }
            }
            fn pb_new() -> Self {
                #zero
            }
            fn pb_size(&self, __number: u32, _enc: #pb::PbEnc) -> usize {
                match self {
                    #none_size
                    #(Self::#idents(__v) => #pb::tag_size(__number + #index)
                        + <#types as #pb::PbSingle>::pb_payload_size(__v, #plain),)*
                }
            }
            fn pb_write(&self, __number: u32, _enc: #pb::PbEnc, __w: &mut #pb::PbWriter) {
                match self {
                    #none_write
                    #(Self::#idents(__v) => {
                        <#types as #pb::PbSingle>::pb_write_payload(__v, #plain, __w);
                        __w.tag(__number + #index, <#types as #pb::PbSingle>::pb_wire(#plain));
                    })*
                }
            }
            fn pb_read(
                __r: &mut #pb::PbReader,
                __end: usize,
                __slot: u32,
                __wire: u8,
                _enc: #pb::PbEnc,
                __out: ::core::option::Option<&mut Self>,
            ) -> ::core::result::Result<bool, #pb::PbError> {
                match __slot {
                    #(#index => {
                        if __wire != <#types as #pb::PbSingle>::pb_wire(#plain) {
                            return ::core::result::Result::Ok(false);
                        }
                        match __out {
                            ::core::option::Option::None => {
                                <#types as #pb::PbSingle>::pb_read_payload(
                                    __r, __end, #plain, ::core::option::Option::None)?
                            }
                            ::core::option::Option::Some(__o) => {
                                // Another alternative was set: this one, from zero.
                                if !matches!(__o, Self::#idents(_)) {
                                    *__o = Self::#idents(<#types as #pb::PbSingle>::pb_zero());
                                }
                                if let Self::#idents(__v) = __o {
                                    <#types as #pb::PbSingle>::pb_read_payload(
                                        __r, __end, #plain, ::core::option::Option::Some(__v))?
                                }
                            }
                        }
                        ::core::result::Result::Ok(true)
                    })*
                    _ => ::core::result::Result::Ok(false),
                }
            }
        }
    })
}
