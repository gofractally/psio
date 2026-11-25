//! `#[derive(View)]`: the view of a record that has one shape in every
//! format (`psio::view`; prototype, gofractally/psio#13).
//!
//! For a record `Order` it writes
//!
//! * `impl ViewShape for Order`: an `Order` reads as `View<'_, Order, F>`;
//! * for each format named in `#[view(...)]` (`pssz`, the default, and
//!   `flatbuf`), `RecordIn` and one `FieldIn` per member, on top of that
//!   format's own view (`#[derive(PsszView)]`, `#[derive(FbView)]`), which
//!   the record must derive too;
//! * `trait OrderFormat`, the bound for code that is generic over the
//!   format: its supertraits are everything reading an `Order` needs;
//! * `trait OrderView`, the members as methods, implemented for
//!   `View<'_, Order, F>` for every `F: OrderFormat`.
//!
//! What `OrderFormat` requires of the members' types is read off their
//! syntax: `Vec<T>` and `[T; N]` are lists, `Option<T>` is what `T` is, the
//! primitive types and `String` need nothing, and any other type `m::Foo`
//! is taken to be a record with a `m::FooFormat` beside it.

use proc_macro::TokenStream;
use proc_macro2::TokenStream as Tokens;
use quote::{format_ident, quote};
use syn::{parse_macro_input, Data, DeriveInput, Fields, GenericArgument, Lit, Meta, NestedMeta, PathArguments, Type};

pub fn view_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    expand(&input).unwrap_or_else(|e| e.to_compile_error()).into()
}

const VALUES: [&str; 12] = ["bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "String"];

/// The path of the psio crate: `#[psio(crate = "path")]`, or `::psio`.
fn krate(input: &DeriveInput) -> syn::Result<syn::Path> {
    for attr in input.attrs.iter().filter(|a| a.path.is_ident("psio")) {
        if let Meta::List(list) = attr.parse_meta()? {
            for item in list.nested {
                if let NestedMeta::Meta(Meta::NameValue(nv)) = item {
                    if nv.path.is_ident("crate") {
                        if let Lit::Str(s) = nv.lit {
                            return s.parse();
                        }
                    }
                }
            }
        }
    }
    Ok(syn::parse_quote!(::psio))
}

/// The formats of `#[view(...)]`: (pssz, flatbuf).
fn formats(input: &DeriveInput) -> syn::Result<(bool, bool)> {
    let mut named = None;
    for attr in input.attrs.iter().filter(|a| a.path.is_ident("view")) {
        let Meta::List(list) = attr.parse_meta()? else {
            return Err(syn::Error::new_spanned(attr, "expected #[view(pssz, flatbuf)]"));
        };
        let (mut pssz, mut flatbuf) = named.unwrap_or((false, false));
        for item in &list.nested {
            match item {
                NestedMeta::Meta(Meta::Path(p)) if p.is_ident("pssz") => pssz = true,
                NestedMeta::Meta(Meta::Path(p)) if p.is_ident("flatbuf") => flatbuf = true,
                other => return Err(syn::Error::new_spanned(other, "a view format is `pssz` or `flatbuf`")),
            }
        }
        named = Some((pssz, flatbuf));
    }
    Ok(named.unwrap_or((true, false)))
}

/// What the format of a position has to satisfy for a member of type `ty`
/// there to be read all the way down.
fn requirements(ty: &Type, k: &syn::Path, out: &mut Vec<Tokens>) -> syn::Result<()> {
    match ty {
        Type::Array(array) => {
            out.push(quote!(#k::view::ReadsList<#ty>));
            requirements(&array.elem, k, out)
        }
        Type::Paren(inner) => requirements(&inner.elem, k, out),
        Type::Group(inner) => requirements(&inner.elem, k, out),
        Type::Path(path) if path.qself.is_none() => {
            let last = path.path.segments.last().expect("a path has a segment");
            let name = last.ident.to_string();
            let argument = || match &last.arguments {
                PathArguments::AngleBracketed(args) if args.args.len() == 1 => match &args.args[0] {
                    GenericArgument::Type(inner) => Some(inner),
                    _ => None,
                },
                _ => None,
            };
            if path.path.segments.len() == 1 && VALUES.contains(&name.as_str()) && last.arguments.is_empty() {
                return Ok(());
            }
            match (name.as_str(), argument()) {
                ("Vec", Some(inner)) => {
                    out.push(quote!(#k::view::ReadsList<#ty>));
                    requirements(inner, k, out)
                }
                ("Option", Some(inner)) => requirements(inner, k, out),
                _ if last.arguments.is_empty() => {
                    // A record: the trait its own derive wrote beside it.
                    let mut format = path.path.clone();
                    let last = format.segments.last_mut().expect("a path has a segment");
                    last.ident = format_ident!("{}Format", last.ident);
                    out.push(quote!(#format));
                    Ok(())
                }
                _ => Err(syn::Error::new_spanned(ty, "psio View (prototype): unsupported member type")),
            }
        }
        _ => Err(syn::Error::new_spanned(ty, "psio View (prototype): unsupported member type")),
    }
}

fn expand(input: &DeriveInput) -> syn::Result<Tokens> {
    let k = krate(input)?;
    let (pssz, flatbuf) = formats(input)?;
    let name = &input.ident;
    let vis = &input.vis;
    if !input.generics.params.is_empty() {
        return Err(syn::Error::new_spanned(&input.generics, "psio View (prototype): a generic record is not supported"));
    }
    let Data::Struct(data) = &input.data else {
        return Err(syn::Error::new_spanned(input, "psio View (prototype): only a struct with named members"));
    };
    let Fields::Named(fields) = &data.fields else {
        return Err(syn::Error::new_spanned(&data.fields, "psio View (prototype): only a struct with named members"));
    };
    if fields.named.is_empty() {
        return Err(syn::Error::new_spanned(&data.fields, "psio View (prototype): a record needs a member"));
    }
    let members: Vec<_> = fields.named.iter().map(|f| (f.ident.clone().expect("named"), &f.ty)).collect();
    let v = quote!(#k::view);
    let format_trait = format_ident!("{}Format", name);
    let view_trait = format_ident!("{}View", name);
    let fb_accessors = format_ident!("{}FbAccessors", name);

    let mut out = quote! {
        impl #v::ViewShape for #name {
            type Shape<'__v, __F: #v::ViewTag> = #v::View<'__v, #name, __F>;
        }
    };

    if pssz {
        out.extend(quote! {
            impl<__F: #k::pssz::PsszFormat> #v::pssz::PsszShape<__F> for #name {
                #[inline]
                fn from_pssz<'__v>(bytes: &'__v [u8]) -> #v::View<'__v, #name, __F> {
                    #v::View::at(bytes)
                }
            }
        });
        // One implementation per format tag: a downstream crate may not
        // implement for every `F: PsszFormat` beside another format.
        let tags = [
            (quote!(), quote!(#k::pssz::Pssz)),
            (quote!(<const __C: usize>), quote!(#k::pssz::PsszUnder<__C>)),
        ];
        for (generics, tag) in &tags {
            out.extend(quote! {
                impl #generics #v::RecordIn<#tag> for #name {
                    type Beneath = <#name as #k::pssz_view::PsszField<#tag, 0>>::Beneath;
                    #[inline]
                    fn validate(bytes: &[u8]) -> ::core::result::Result<(), #k::pssz::PsszError> {
                        #v::pssz::validate::<#name, #tag>(bytes)
                    }
                    #[inline]
                    fn root(bytes: &[u8]) -> &[u8] {
                        bytes
                    }
                }
            });
            for (index, (_, ty)) in members.iter().enumerate() {
                out.extend(quote! {
                    impl #generics #v::FieldIn<#tag, #index> for #name {
                        type Type = #ty;
                        #[inline]
                        fn field<'__v>(record: &'__v [u8])
                            -> <#ty as #v::ViewShape>::Shape<'__v, <#name as #v::RecordIn<#tag>>::Beneath>
                        where
                            '__v: '__v,
                        {
                            #v::pssz::field::<#name, #tag, #ty, #index>(record)
                        }
                    }
                });
            }
        }
    }

    if flatbuf {
        let tag = quote!(#k::flatbuf::Flatbuf);
        out.extend(quote! {
            impl #v::flatbuf::FbShape for #name {
                #[inline]
                fn from_fb<'__v>(view: #k::flatbuf::FbView<'__v, #name>) -> #v::View<'__v, #name, #tag>
                where
                    '__v: '__v,
                {
                    #v::flatbuf::record(view)
                }
            }
            impl #v::RecordIn<#tag> for #name {
                type Beneath = #tag;
                #[inline]
                fn validate(bytes: &[u8]) -> ::core::result::Result<(), #k::flatbuf::FbError> {
                    #v::flatbuf::validate::<#name>(bytes)
                }
                #[inline]
                fn root(bytes: &[u8]) -> #v::flatbuf::FbCursor<'_> {
                    #v::flatbuf::root::<#name>(bytes)
                }
            }
        });
        for (index, (member, ty)) in members.iter().enumerate() {
            out.extend(quote! {
                impl #v::FieldIn<#tag, #index> for #name {
                    type Type = #ty;
                    #[inline]
                    fn field<'__v>(record: #v::flatbuf::FbCursor<'__v>) -> <#ty as #v::ViewShape>::Shape<'__v, #tag>
                    where
                        '__v: '__v,
                    {
                        <#ty as #v::flatbuf::FbShape>::from_fb(
                            <#k::flatbuf::FbView<'__v, #name> as #fb_accessors<'__v>>::#member(
                                &#v::flatbuf::native::<#name>(record),
                            ),
                        )
                    }
                }
            });
        }
    }

    // The bound for code generic over the format.
    let mut beneath = Vec::new();
    for (_, ty) in &members {
        requirements(ty, &k, &mut beneath)?;
    }
    let beneath_bound = if beneath.is_empty() { quote!(#v::ViewTag) } else { quote!(#(#beneath)+*) };
    let indices: Vec<usize> = (0..members.len()).collect();
    let types: Vec<_> = members.iter().map(|(_, ty)| *ty).collect();
    let format_doc = format!("A format a [`{name}`] is read in: the bound of code generic over the format.");
    out.extend(quote! {
        #[doc = #format_doc]
        #vis trait #format_trait:
            #v::ViewTag
            + #v::RecordFormat<#name, Beneath: #beneath_bound>
            #(+ #v::ReadsField<#name, #indices, Type = #types>)*
        {
        }
        impl<__F> #format_trait for __F
        where
            __F: #v::ViewTag + #v::RecordFormat<#name> #(+ #v::ReadsField<#name, #indices, Type = #types>)*,
            <__F as #v::RecordFormat<#name>>::Beneath: #beneath_bound,
        {
        }
    });

    // The members.
    let mut declarations = Vec::new();
    let mut methods = Vec::new();
    for (index, (member, ty)) in members.iter().enumerate() {
        let doc = format!("The member `{}`.", member);
        let shape = quote!(<#ty as #v::ViewShape>::Shape<'__v, <__F as #v::RecordFormat<#name>>::Beneath>);
        declarations.push(quote!(#[doc = #doc] fn #member(&self) -> #shape;));
        methods.push(quote! {
            #[inline]
            fn #member(&self) -> #shape {
                <__F as #v::ReadsField<#name, #index>>::field(#v::View::cursor(self))
            }
        });
    }
    let view_doc = format!("The members of a [`{name}`] read in place, in any format: implemented for `View<{name}, F>`.");
    out.extend(quote! {
        #[doc = #view_doc]
        #vis trait #view_trait<'__v, __F: #format_trait> {
            #(#declarations)*
        }
        impl<'__v, __F: #format_trait> #view_trait<'__v, __F> for #v::View<'__v, #name, __F> {
            #(#methods)*
        }
    });
    Ok(out)
}
