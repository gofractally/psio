//! `#[derive(Key)]`: the memcmp-sortable `key` format (`psio::key`).
//!
//! * A struct is a composite key: its fields' keys concatenated in
//!   declaration order, exactly the key of the tuple of its fields. It also
//!   implements `CompositeKey`, so `psio::key::key_prefix` accepts it.
//! * A field-less `#[repr(int)]` enum (optionally with a `#[pssz(other)]`
//!   catch-all) is its underlying integer, exactly as in PSSZ and as a C++
//!   enum: key order is the order of the values. Its names exist in JSON
//!   only.
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
    let k = quote!(::psio::key);
    let mut pack_generics = input.generics.clone();
    for param in pack_generics.type_params_mut() {
        param.bounds.push(syn::parse_quote!(::psio::key::KeyPack));
    }
    let mut unpack_generics = input.generics.clone();
    for param in unpack_generics.type_params_mut() {
        param.bounds.push(syn::parse_quote!(::psio::key::KeyUnpack));
    }
    let (pack_impl, ty_generics, where_clause) = pack_generics.split_for_impl();
    let (unpack_impl, _, _) = unpack_generics.split_for_impl();
    let (plain_impl, _, _) = input.generics.split_for_impl();

    match &input.data {
        Data::Struct(data) => {
            let members: Vec<syn::Member> = match &data.fields {
                Fields::Named(fields) => fields
                    .named
                    .iter()
                    .map(|f| syn::Member::Named(f.ident.clone().unwrap()))
                    .collect(),
                Fields::Unnamed(fields) => {
                    (0..fields.unnamed.len()).map(|i| syn::Member::Unnamed(i.into())).collect()
                }
                Fields::Unit => Vec::new(),
            };
            let types: Vec<&syn::Type> = data.fields.iter().map(|f| &f.ty).collect();
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
                }
                impl #unpack_impl #k::KeyUnpack for #name #ty_generics #where_clause {
                    #[allow(unused_variables)]
                    fn key_unpack(r: &mut #k::KeyReader<'_>) -> #k::KeyResult<Self> {
                        ::core::result::Result::Ok(Self {
                            #(#members: <#types as #k::KeyUnpack>::key_unpack(r)?,)*
                        })
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
                        #k::KeyError("key: value is not an enumerator"))),
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
                }
                impl #plain_impl #k::KeyUnpack for #name #ty_generics #where_clause {
                    fn key_unpack(r: &mut #k::KeyReader<'_>) -> #k::KeyResult<Self> {
                        #preamble
                        let __value = <#repr as #k::KeyUnpack>::key_unpack(r)?;
                        #(if __value == #values {
                            return ::core::result::Result::Ok(Self::#variants);
                        })*
                        #unpack_other
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
                }
                impl #unpack_impl #k::KeyUnpack for #name #ty_generics #where_clause {
                    fn key_unpack(r: &mut #k::KeyReader<'_>) -> #k::KeyResult<Self> {
                        match r.byte()? {
                            #(#indexes => ::core::result::Result::Ok(
                                Self::#idents(<#types as #k::KeyUnpack>::key_unpack(r)?)),)*
                            _ => ::core::result::Result::Err(
                                #k::KeyError("key: variant index out of range")),
                        }
                    }
                }
            })
        }
        Data::Union(_) => Err(syn::Error::new_spanned(name, "derive(Key): unions are not keys")),
    }
}
