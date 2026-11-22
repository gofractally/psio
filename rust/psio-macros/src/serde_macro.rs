//! `#[derive(Serde)]`: `serde::Serialize` and `serde::Deserialize` for a
//! type that is `JsonText`, through `psio::serde_bridge`. The serde impls
//! read and write the type's IJson text, so the type has one description.

use crate::attrs::{self, Derive};
use proc_macro::TokenStream;
use quote::quote;
use syn::{parse_macro_input, parse_quote, DeriveInput};

pub fn derive(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    match expand(&input) {
        Ok(tokens) => tokens.into(),
        Err(err) => err.to_compile_error().into(),
    }
}

fn expand(input: &DeriveInput) -> syn::Result<proc_macro2::TokenStream> {
    let k = attrs::item(input, Derive::Serde)?.krate();
    let name = &input.ident;
    // A type parameter is `JsonText` where the `JsonText` derive needs it to
    // be.
    let mut generics = input.generics.clone();
    for param in generics.type_params_mut() {
        param.bounds.push(parse_quote!(#k::json::JsonText));
    }
    let (ser_impl, ty_generics, where_clause) = generics.split_for_impl();
    let mut de_generics = generics.clone();
    de_generics.params.insert(0, parse_quote!('__psio_de));
    let (de_impl, _, _) = de_generics.split_for_impl();
    Ok(quote! {
        impl #ser_impl #k::__serde::Serialize for #name #ty_generics #where_clause {
            fn serialize<__S: #k::__serde::Serializer>(
                &self,
                serializer: __S,
            ) -> ::core::result::Result<__S::Ok, __S::Error> {
                #k::serde_bridge::serialize(self, serializer)
            }
        }
        impl #de_impl #k::__serde::Deserialize<'__psio_de> for #name #ty_generics #where_clause {
            fn deserialize<__D: #k::__serde::Deserializer<'__psio_de>>(
                deserializer: __D,
            ) -> ::core::result::Result<Self, __D::Error> {
                #k::serde_bridge::deserialize(deserializer)
            }
        }
    })
}
