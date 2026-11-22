//! `#[derive(Key)]`: the memcmp-sortable `key` format (`psio::key`).
//!
//! * A struct is a composite key: its fields' keys concatenated in
//!   declaration order, exactly the key of the tuple of its fields. It also
//!   implements `CompositeKey`, so `psio::key::key_prefix` accepts it.
//! * A field-less `#[repr(int)]` enum (optionally with a `#[psio(other)]`
//!   catch-all) is its underlying integer, exactly as in PSSZ and as a C++
//!   enum: key order is the order of the values. Its names exist in JSON
//!   only.
//! * A struct of one member marked `#[psio(transparent)]` (or
//!   `#[serde(transparent)]`) is that member: same key, and a composite key
//!   when the member is a tuple.
//! * `#[psio(invariant = "path")]` (a `fn(&Self) -> Result<(), E>` with
//!   `E: Into<Cow<'static, str>>`): `try_to_key` / `to_key` refuse a value that violates it and the
//!   readers reject its key with the invariant's message.
//! * Any other enum, whose variants each hold one unnamed field, is a
//!   variant: one byte holding the variant's index, then the payload's key
//!   (C++ `std::variant`). At most 256 variants.

use proc_macro::TokenStream;
use proc_macro2::TokenStream as TokenStream2;
use quote::quote;
use syn::{parse_macro_input, Data, DeriveInput, Fields};

pub fn derive(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    match expand(&input) {
        Ok(tokens) => tokens.into(),
        Err(err) => err.to_compile_error().into(),
    }
}

fn expand(input: &DeriveInput) -> syn::Result<TokenStream2> {
    let name = &input.ident;
    let shared = crate::attrs::item(input, crate::attrs::Derive::Key)?;
    let krate = shared.krate();
    let k = quote!(#krate::key);
    let mut pack_generics = input.generics.clone();
    for param in pack_generics.type_params_mut() {
        param.bounds.push(syn::parse_quote!(#k::KeyPack));
    }
    let mut unpack_generics = input.generics.clone();
    for param in unpack_generics.type_params_mut() {
        param.bounds.push(syn::parse_quote!(#k::KeyUnpack));
    }
    let (pack_impl, ty_generics, where_clause) = pack_generics.split_for_impl();
    let (unpack_impl, _, _) = unpack_generics.split_for_impl();
    let (plain_impl, _, _) = input.generics.split_for_impl();
    // The type's own invariant, after those of what it holds.
    let own_check = |value: TokenStream2| match &shared.invariant {
        Some(path) => quote! {
            if let ::core::result::Result::Err(__why) = #path(#value) {
                return ::core::result::Result::Err(#k::KeyError(::core::convert::Into::into(__why)));
            }
        },
        None => quote!(),
    };
    let check_self = own_check(quote!(self));
    let check_value = own_check(quote!(&__value));

    match &input.data {
        Data::Struct(data) if shared.transparent => {
            let members = crate::attrs::members(&data.fields)?;
            if members.len() != 1 {
                return Err(syn::Error::new_spanned(
                    &data.fields,
                    "a transparent type has exactly one member",
                ));
            }
            let ty = &members[0].field.ty;
            let member = &members[0].member;
            let construct =
                crate::attrs::construct(quote!(Self), &data.fields, &[quote!(__inner)])?;
            // A transparent wrapper of a composite key is that composite key.
            let composite = match ty {
                syn::Type::Tuple(_) => quote! {
                    impl #plain_impl #k::CompositeKey for #name #ty_generics #where_clause {
                        type Elements = #ty;
                    }
                },
                _ => quote!(),
            };
            Ok(quote! {
                impl #pack_impl #k::KeyPack for #name #ty_generics #where_clause {
                    const FIXED_SIZE: ::core::option::Option<usize> = <#ty as #k::KeyPack>::FIXED_SIZE;
                    fn key_size(&self) -> usize {
                        #k::KeyPack::key_size(&self.#member)
                    }
                    fn key_pack(&self, out: &mut ::std::vec::Vec<u8>) {
                        #k::KeyPack::key_pack(&self.#member, out)
                    }
                    fn key_check(&self) -> #k::KeyResult<()> {
                        #k::KeyPack::key_check(&self.#member)?;
                        #check_self
                        ::core::result::Result::Ok(())
                    }
                }
                impl #unpack_impl #k::KeyUnpack for #name #ty_generics #where_clause {
                    fn key_unpack(r: &mut #k::KeyReader<'_>) -> #k::KeyResult<Self> {
                        let __inner = <#ty as #k::KeyUnpack>::key_unpack(r)?;
                        let __value = #construct;
                        #check_value
                        ::core::result::Result::Ok(__value)
                    }
                }
                #composite
            })
        }
        Data::Struct(data) => {
            // The key is that of the members: a skipped field is in no key.
            let kept = crate::attrs::members(&data.fields)?;
            let members: Vec<&syn::Member> = kept.iter().map(|m| &m.member).collect();
            let types: Vec<&syn::Type> = kept.iter().map(|m| &m.field.ty).collect();
            let unpacked: Vec<TokenStream2> = types
                .iter()
                .map(|ty| quote!(<#ty as #k::KeyUnpack>::key_unpack(r)?))
                .collect();
            let construct = crate::attrs::construct(quote!(Self), &data.fields, &unpacked)?;
            // A member with bounds: the key is the plain type's, checked
            // against the annotation by the writers and the reader.
            let mut bound_checks = Vec::new();
            for m in &kept {
                if let Some(crate::attrs::Annotated { own, spec }) =
                    crate::attrs::annotated(m.field, &m.attrs, &krate)?
                {
                    let member = &m.member;
                    bound_checks.push(quote! {
                        if #krate::pssz::bounds::check::<#own, #spec>(&__bounded.#member).is_err() {
                            return ::core::result::Result::Err(#k::KeyError::new("key: member length_bound violated"));
                        }
                    });
                }
            }
            Ok(quote! {
                impl #pack_impl #k::KeyPack for #name #ty_generics #where_clause {
                    const FIXED_SIZE: ::core::option::Option<usize> = #k::record_fixed_size(
                        &[#(<#types as #k::KeyPack>::FIXED_SIZE),*]);
                    fn key_size(&self) -> usize {
                        0 #(+ #k::KeyPack::key_size(&self.#members))*
                    }
                    #[allow(unused_variables)]
                    fn key_pack(&self, out: &mut ::std::vec::Vec<u8>) {
                        #(#k::KeyPack::key_pack(&self.#members, out);)*
                    }
                    fn key_check(&self) -> #k::KeyResult<()> {
                        #(#k::KeyPack::key_check(&self.#members)?;)*
                        { let __bounded = self; #(#bound_checks)* }
                        #check_self
                        ::core::result::Result::Ok(())
                    }
                }
                impl #unpack_impl #k::KeyUnpack for #name #ty_generics #where_clause {
                    #[allow(unused_variables)]
                    fn key_unpack(r: &mut #k::KeyReader<'_>) -> #k::KeyResult<Self> {
                        let __value = #construct;
                        { let __bounded = &__value; #(#bound_checks)* }
                        #check_value
                        ::core::result::Result::Ok(__value)
                    }
                }
                impl #plain_impl #k::CompositeKey for #name #ty_generics #where_clause {
                    type Elements = (#(#types,)*);
                }
            })
        }
        Data::Enum(_) if crate::c_enum::analyze(input)?.is_some() => {
            let shape = crate::c_enum::analyze(input)?.expect("checked");
            let Some(repr) = shape.repr.clone() else {
                return Err(syn::Error::new_spanned(
                    name,
                    "derive(Key): a field-less enum is keyed as its underlying integer: add \
                     #[repr(u8)] (or u16/u32/u64/i8/i16/i32/i64)",
                ));
            };
            let variants: Vec<_> = shape.declared.iter().map(|v| &v.ident).collect();
            let (preamble, values) = shape.values();
            let (pack_other, unpack_other) = match shape.other {
                Some(other) => {
                    let other = &other.ident;
                    (
                        quote!(Self::#other(__n) => *__n,),
                        quote!(::core::result::Result::Ok(Self::#other(__value))),
                    )
                }
                None => (
                    quote!(),
                    quote!(::core::result::Result::Err(
                        #k::KeyError::new("key: value is not an enumerator"))),
                ),
            };
            Ok(quote! {
                impl #plain_impl #k::KeyPack for #name #ty_generics #where_clause {
                    const FIXED_SIZE: ::core::option::Option<usize> =
                        <#repr as #k::KeyPack>::FIXED_SIZE;
                    fn key_size(&self) -> usize {
                        ::core::mem::size_of::<#repr>()
                    }
                    fn key_pack(&self, out: &mut ::std::vec::Vec<u8>) {
                        #preamble
                        let __value: #repr = match self {
                            #(Self::#variants => #values,)*
                            #pack_other
                        };
                        #k::KeyPack::key_pack(&__value, out);
                    }
                    fn key_check(&self) -> #k::KeyResult<()> {
                        #check_self
                        ::core::result::Result::Ok(())
                    }
                }
                impl #plain_impl #k::KeyUnpack for #name #ty_generics #where_clause {
                    fn key_unpack(r: &mut #k::KeyReader<'_>) -> #k::KeyResult<Self> {
                        let __value: Self = (|| -> #k::KeyResult<Self> {
                            #preamble
                            let __value = <#repr as #k::KeyUnpack>::key_unpack(r)?;
                            #(if __value == #values {
                                return ::core::result::Result::Ok(Self::#variants);
                            })*
                            #unpack_other
                        })()?;
                        #check_value
                        ::core::result::Result::Ok(__value)
                    }
                }
            })
        }
        Data::Enum(data) => {
            if data.variants.len() > 256 {
                return Err(syn::Error::new_spanned(
                    name,
                    "derive(Key): a variant has at most 256 alternatives",
                ));
            }
            let mut idents = Vec::new();
            let mut types = Vec::new();
            let mut indexes = Vec::new();
            for (index, variant) in data.variants.iter().enumerate() {
                match &variant.fields {
                    Fields::Unnamed(fields) if fields.unnamed.len() == 1 => {
                        idents.push(&variant.ident);
                        types.push(&fields.unnamed[0].ty);
                        indexes.push(index as u8);
                    }
                    _ => {
                        return Err(syn::Error::new_spanned(
                            variant,
                            "derive(Key): each enum variant must hold exactly one unnamed field",
                        ))
                    }
                }
            }
            Ok(quote! {
                impl #pack_impl #k::KeyPack for #name #ty_generics #where_clause {
                    fn key_size(&self) -> usize {
                        match self {
                            #(Self::#idents(value) => 1 + #k::KeyPack::key_size(value),)*
                        }
                    }
                    fn key_pack(&self, out: &mut ::std::vec::Vec<u8>) {
                        match self {
                            #(Self::#idents(value) => {
                                out.push(#indexes);
                                #k::KeyPack::key_pack(value, out);
                            })*
                        }
                    }
                    fn key_check(&self) -> #k::KeyResult<()> {
                        match self {
                            #(Self::#idents(value) => #k::KeyPack::key_check(value)?,)*
                        }
                        #check_self
                        ::core::result::Result::Ok(())
                    }
                }
                impl #unpack_impl #k::KeyUnpack for #name #ty_generics #where_clause {
                    fn key_unpack(r: &mut #k::KeyReader<'_>) -> #k::KeyResult<Self> {
                        let __value = match r.byte()? {
                            #(#indexes => Self::#idents(<#types as #k::KeyUnpack>::key_unpack(r)?),)*
                            _ => return ::core::result::Result::Err(
                                #k::KeyError::new("key: variant index out of range")),
                        };
                        #check_value
                        ::core::result::Result::Ok(__value)
                    }
                }
            })
        }
        Data::Union(_) => Err(syn::Error::new_spanned(name, "derive(Key): unions are not keys")),
    }
}
