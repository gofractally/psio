//! A C-like enum as the PSSZ, schema and JSON derives see it: field-less
//! variants over a `#[repr(int)]`, optionally with one catch-all
//!
//! ```ignore
//! #[repr(i32)]
//! enum Status {
//!     Pending = -1,
//!     Active = 2,
//!     #[psio(other)]
//!     Unknown(i32),
//! }
//! ```
//!
//! The catch-all holds a wire value that is no declared enumerator, so an
//! old reader keeps reading data from a writer that has since added
//! enumerators. It has no name in JSON and is not an enumerator in the
//! schema. A value equal to a declared enumerator always decodes to the
//! declared variant, and writing `Unknown(n)` for a declared `n` is an
//! error: each value has one in-memory representation.

use proc_macro2::TokenStream as Tokens;
use quote::{format_ident, quote};
use syn::{Data, DeriveInput, Fields, Meta, NestedMeta};

pub(crate) fn int_repr(input: &DeriveInput) -> Option<syn::Ident> {
    for attr in &input.attrs {
        if !attr.path.is_ident("repr") {
            continue;
        }
        if let Ok(Meta::List(list)) = attr.parse_meta() {
            for nested in &list.nested {
                if let NestedMeta::Meta(Meta::Path(path)) = nested {
                    if let Some(ident) = path.get_ident() {
                        if matches!(
                            ident.to_string().as_str(),
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

/// Whether the variant carries `#[psio(other)]`.
pub(crate) fn is_other(variant: &syn::Variant) -> syn::Result<bool> {
    Ok(crate::attrs::variant(variant)?.other)
}

pub(crate) struct CEnum<'a> {
    /// `None`: the enum has no integer `#[repr]` (the caller reports it).
    pub(crate) repr: Option<syn::Ident>,
    all: Vec<&'a syn::Variant>,
    /// The field-less variants, in declaration order.
    pub(crate) declared: Vec<&'a syn::Variant>,
    /// The `#[psio(other)]` variant.
    pub(crate) other: Option<&'a syn::Variant>,
}

/// `None` when the item is not a C-like enum (a struct, or an enum whose
/// variants carry payloads).
pub(crate) fn analyze(input: &DeriveInput) -> syn::Result<Option<CEnum<'_>>> {
    let Data::Enum(data) = &input.data else { return Ok(None) };
    let repr = int_repr(input);
    let mut declared = Vec::new();
    let mut other = None;
    let mut payloads = false;
    for variant in &data.variants {
        if is_other(variant)? {
            if other.is_some() {
                return Err(syn::Error::new_spanned(
                    variant,
                    "an enum has at most one #[psio(other)] variant",
                ));
            }
            let payload = match &variant.fields {
                Fields::Unnamed(fields) if fields.unnamed.len() == 1 => &fields.unnamed[0].ty,
                _ => {
                    return Err(syn::Error::new_spanned(
                        variant,
                        "the #[psio(other)] variant holds exactly one unnamed field of the \
                         enum's #[repr] type",
                    ))
                }
            };
            let Some(repr) = &repr else {
                return Err(syn::Error::new_spanned(
                    &input.ident,
                    "#[psio(other)] needs #[repr(u8)] (or u16/u32/u64/i8/i16/i32/i64) on the enum",
                ));
            };
            if quote!(#payload).to_string() != repr.to_string() {
                return Err(syn::Error::new_spanned(
                    payload,
                    "the #[psio(other)] payload must be the enum's #[repr] type",
                ));
            }
            other = Some(variant);
        } else if matches!(variant.fields, Fields::Unit) {
            declared.push(variant);
        } else {
            payloads = true;
        }
    }
    if payloads {
        if let Some(other) = other {
            return Err(syn::Error::new_spanned(
                other,
                "#[psio(other)] applies to an otherwise field-less enum",
            ));
        }
        return Ok(None);
    }
    if data.variants.is_empty() {
        return Ok(None);
    }
    Ok(Some(CEnum { repr, all: data.variants.iter().collect(), declared, other }))
}

impl CEnum<'_> {
    /// Statements to put before any use of the values, and the value of each
    /// declared variant as an expression of the repr type.
    ///
    /// Without a catch-all the enum is field-less and `Self::V as repr` is
    /// the value. With one, `as` is not available, so the discriminants are
    /// recomputed as constants by Rust's own rule: the explicit expression,
    /// else the previous variant's plus one, else zero.
    pub(crate) fn values(&self) -> (Tokens, Vec<Tokens>) {
        let repr = self.repr.as_ref().expect("checked by the caller");
        if self.other.is_none() {
            let values = self
                .declared
                .iter()
                .map(|v| {
                    let ident = &v.ident;
                    quote!((Self::#ident as #repr))
                })
                .collect();
            return (quote!(), values);
        }
        let mut consts = Vec::new();
        let mut values = Vec::new();
        let mut previous: Option<syn::Ident> = None;
        for (index, variant) in self.all.iter().enumerate() {
            let is_other = self.other.is_some_and(|o| std::ptr::eq(o, *variant));
            let name = format_ident!("__PSIO_ENUM_VALUE_{}", index);
            // The catch-all's own discriminant is never on the wire; it is
            // only needed when a later variant counts on from it.
            let value = match (&variant.discriminant, &previous) {
                (Some((_, expr)), _) => quote!(#expr),
                (None, Some(previous)) => quote!(#previous + 1),
                (None, None) => quote!(0),
            };
            consts.push(quote!(#[allow(dead_code)] const #name: #repr = #value;));
            if !is_other {
                values.push(quote!(#name));
            }
            previous = Some(name);
        }
        (quote!(#(#consts)*), values)
    }

    pub(crate) fn signed(&self) -> bool {
        self.repr.as_ref().is_some_and(|r| r.to_string().starts_with('i'))
    }
}
