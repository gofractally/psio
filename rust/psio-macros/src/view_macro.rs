//! `#[derive(View)]`: the view of a type that has one shape in every format
//! (`psio::view`; gofractally/psio#13).
//!
//! For a record `Order` it writes
//!
//! * `struct OrderView<'a, F>`: what an `Order` reads as in the format `F`
//!   (`View<'a, Order, F>` names it), with `new`, `trusted`, `unpack` and
//!   one method per member, each as visible as the member is;
//! * `impl ViewShape for Order`: its members, as a list of their types (for
//!   a member with a bound, the type the bound describes);
//! * for each format named in `#[view(...)]` (`pssz`, the default, and
//!   `flatbuf`), `ViewIn` for that format's family and one reader per
//!   member, on top of the format's own view (`#[derive(PsszView)]`,
//!   `#[derive(FbView)]`), which the record must derive too.
//!
//! An enum without payloads reads as its value. An enum with payloads reads
//! as an enum `<Name>View` with one alternative per variant holding what
//! the payload reads as: a view to `match` on.
//!
//! The derive never asks what kind of type a member is: it names the type
//! and the traits answer. `#[view(name = "Name")]` names the struct or enum
//! it writes.

use crate::attrs;
use crate::c_enum;
use proc_macro::TokenStream;
use proc_macro2::TokenStream as Tokens;
use quote::{format_ident, quote};
use syn::{parse_macro_input, Data, DeriveInput, Fields, Lit, Meta, NestedMeta, Type};

pub fn view_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    expand(&input).unwrap_or_else(|e| e.to_compile_error()).into()
}

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

/// Whether the tokens of a type name `ident`.
fn mentions(tokens: Tokens, ident: &syn::Ident) -> bool {
    tokens.into_iter().any(|tree| match tree {
        proc_macro2::TokenTree::Ident(found) => found == *ident || found == "Self",
        proc_macro2::TokenTree::Group(group) => mentions(group.stream(), ident),
        _ => false,
    })
}

/// What `#[view(...)]` on the type says.
struct Said {
    pssz: bool,
    flatbuf: bool,
    /// `no_debug`: the view of an enum with payloads is not `Debug`.
    no_debug: bool,
    /// `name = "Name"`: the struct or enum the derive writes.
    name: Option<syn::Ident>,
}

fn said(input: &DeriveInput) -> syn::Result<Said> {
    let mut formats = None;
    let mut name = None;
    let mut no_debug = false;
    for attr in input.attrs.iter().filter(|a| a.path.is_ident("view")) {
        let usage = "expected #[view(pssz, flatbuf)] or #[view(name = \"Name\")]";
        let Meta::List(list) = attr.parse_meta()? else {
            return Err(syn::Error::new_spanned(attr, usage));
        };
        for item in &list.nested {
            match item {
                NestedMeta::Meta(Meta::Path(p)) if p.is_ident("pssz") => formats.get_or_insert((false, false)).0 = true,
                NestedMeta::Meta(Meta::Path(p)) if p.is_ident("flatbuf") => formats.get_or_insert((false, false)).1 = true,
                NestedMeta::Meta(Meta::Path(p)) if p.is_ident("no_debug") => no_debug = true,
                NestedMeta::Meta(Meta::NameValue(nv)) if nv.path.is_ident("name") => match &nv.lit {
                    Lit::Str(s) => name = Some(s.parse()?),
                    _ => return Err(syn::Error::new_spanned(nv, usage)),
                },
                other => return Err(syn::Error::new_spanned(other, usage)),
            }
        }
    }
    let (pssz, flatbuf) = formats.unwrap_or((true, false));
    Ok(Said { pssz, flatbuf, no_debug, name })
}

fn expand(input: &DeriveInput) -> syn::Result<Tokens> {
    let k = krate(input)?;
    let said = said(input)?;
    match &input.data {
        Data::Struct(data) => record(input, &data.fields, &k, &said),
        Data::Enum(data) => match c_enum::analyze(input)? {
            Some(_) => value_enum(input, &k, &said),
            None => variants(input, data, &k, &said),
        },
        Data::Union(data) => Err(syn::Error::new_spanned(data.union_token, "psio View: not for a union")),
    }
}

/// The generic parameters of the type, as they are declared and as they
/// are used, each followed by a comma: to put before the parameters the
/// derive adds.
struct Params {
    lifetimes: Tokens,
    declared: Tokens,
    used_lifetimes: Tokens,
    used: Tokens,
    /// Whether there is a type or const parameter.
    generic: bool,
    predicates: Vec<Tokens>,
}

fn params(input: &DeriveInput) -> Params {
    let (mut lifetimes, mut declared, mut used_lifetimes, mut used) = (quote!(), quote!(), quote!(), quote!());
    let mut generic = false;
    for param in &input.generics.params {
        match param {
            syn::GenericParam::Lifetime(l) => {
                let lifetime = &l.lifetime;
                lifetimes.extend(quote!(#l,));
                used_lifetimes.extend(quote!(#lifetime,));
            }
            syn::GenericParam::Type(t) => {
                generic = true;
                let (ident, bounds) = (&t.ident, &t.bounds);
                declared.extend(quote!(#ident: #bounds,));
                used.extend(quote!(#ident,));
            }
            syn::GenericParam::Const(c) => {
                generic = true;
                let (ident, ty) = (&c.ident, &c.ty);
                declared.extend(quote!(const #ident: #ty,));
                used.extend(quote!(#ident,));
            }
        }
    }
    let predicates = input
        .generics
        .where_clause
        .iter()
        .flat_map(|w| w.predicates.iter().map(|p| quote!(#p)))
        .collect();
    Params { lifetimes, declared, used_lifetimes, used, generic, predicates }
}

/// The marker types that name the members of a type from one of them on
/// (`ListAt`) and their readers (`ReaderAt`): `types` are the members.
fn markers(k: &syn::Path, vis: &syn::Visibility, at: &syn::Ident, p: &Params, this: &Tokens, types: &[Type], bounds: &[Tokens]) -> Tokens {
    let v = quote!(#k::view);
    let d = quote!(#k::view::__derive);
    let (lifetimes, declared, used_lifetimes, used) = (&p.lifetimes, &p.declared, &p.used_lifetimes, &p.used);
    let predicates = &p.predicates;
    let mut out = quote! {
        #[doc(hidden)]
        #[allow(non_camel_case_types)]
        #vis struct #at<#lifetimes #declared const __I: usize>(#d::PhantomData<fn() -> #this>);
    };
    for index in 0..=types.len() {
        let list = match types.get(index) {
            Some(ty) => {
                let next = index + 1;
                quote!((#ty, <#at<#used_lifetimes #used #next> as #d::ListAt>::L))
            }
            None => quote!(()),
        };
        out.extend(quote! {
            impl<#lifetimes #declared> #d::ListAt for #at<#used_lifetimes #used #index>
            where
                #(#predicates,)*
                #(#bounds,)*
            {
                type L = #list;
            }
        });
        let reader = if index == 0 {
            quote!(__R)
        } else {
            let previous = index - 1;
            quote! {
                <<#at<#used_lifetimes #used #previous> as #d::ReaderAt<__F, __R>>::Reader
                    as #v::FieldList<__F, <#at<#used_lifetimes #used #previous> as #d::ListAt>::L>>::Tail
            }
        };
        out.extend(quote! {
            impl<#lifetimes #declared __F, __R> #d::ReaderAt<__F, __R> for #at<#used_lifetimes #used #index>
            where
                __F: #v::ViewTag,
                __R: #v::FieldList<__F, <#at<#used_lifetimes #used 0usize> as #d::ListAt>::L>,
                #(#predicates,)*
                #(#bounds,)*
            {
                type Reader = #reader;
            }
        });
    }
    out
}

// ── Records ─────────────────────────────────────────────────────────────────

fn record(input: &DeriveInput, fields: &Fields, k: &syn::Path, said: &Said) -> syn::Result<Tokens> {
    let name = &input.ident;
    let vis = &input.vis;
    let v = quote!(#k::view);
    let d = quote!(#k::view::__derive);
    let members = attrs::members(fields)?;
    if members.is_empty() {
        return Err(syn::Error::new_spanned(fields, "psio View: a record needs a member"));
    }
    if said.flatbuf && !matches!(fields, Fields::Named(_)) {
        return Err(syn::Error::new_spanned(
            fields,
            "psio View: the FlatBuffers view of a record has accessors for named members only; \
             name the members or leave `flatbuf` out of #[view(...)]",
        ));
    }
    let p = params(input);
    if said.flatbuf && (p.generic || !p.lifetimes.is_empty()) {
        return Err(syn::Error::new_spanned(
            &input.generics,
            "psio View: the FlatBuffers derives have no record with generic parameters; leave `flatbuf` out of #[view(...)]",
        ));
    }
    let (lifetimes, declared, used_lifetimes, used) = (&p.lifetimes, &p.declared, &p.used_lifetimes, &p.used);
    let predicates = &p.predicates;
    let this = quote!(#name<#used_lifetimes #used>);
    let view = said.name.clone().unwrap_or_else(|| format_ident!("{}View", name));
    let at = format_ident!("__{}ViewAt", name);
    let fb_accessors = format_ident!("{}FbAccessors", name);

    // A member: its accessor, how visible it is, and the type the view has
    // for it (the one PSSZ has: for a member with a bound, the type the
    // annotation describes).
    struct Member<'a> {
        method: syn::Ident,
        native: Option<&'a syn::Ident>,
        vis: &'a syn::Visibility,
        ty: Type,
    }
    let mut list = Vec::new();
    for m in &members {
        let (method, native) = match &m.member {
            syn::Member::Named(id) => (id.clone(), Some(m.field.ident.as_ref().expect("named"))),
            syn::Member::Unnamed(index) => (format_ident!("field_{}", index.index), None),
        };
        if ["new", "trusted", "unpack"].contains(&method.to_string().as_str()) {
            return Err(syn::Error::new_spanned(
                &m.field.ident,
                format!("psio View: a member named `{method}` would hide the view's own `{method}`"),
            ));
        }
        let ty = match attrs::annotated(m.field, &m.attrs, k)? {
            Some(annotated) => annotated.described(k),
            None => m.field.ty.clone(),
        };
        list.push(Member { method, native, vis: &m.field.vis, ty });
    }
    let types: Vec<Type> = list.iter().map(|m| m.ty.clone()).collect();
    // With parameters, what the members' types have to be is not known
    // here: it is required.
    let shape_bounds: Vec<Tokens> =
        if p.generic { types.iter().map(|ty| quote!(#ty: #v::ViewShape)).collect() } else { Vec::new() };

    let mut out = markers(k, vis, &at, &p, &this, &types, &shape_bounds);
    let members_list = quote!(<#at<#used_lifetimes #used 0usize> as #d::ListAt>::L);
    let doc = format!("What a [`{name}`] reads as in the format `__F`: `View<'_, {name}, __F>`. One method per member.");
    out.extend(quote! {
        #[doc = #doc]
        #vis struct #view<'__v, #lifetimes #declared __F: #v::ViewTag> {
            cursor: #v::Cursor<'__v, __F>,
            _type: #d::PhantomData<fn() -> #this>,
        }
        impl<'__v, #lifetimes #declared __F: #v::ViewTag> ::core::clone::Clone for #view<'__v, #used_lifetimes #used __F> {
            fn clone(&self) -> Self {
                *self
            }
        }
        impl<'__v, #lifetimes #declared __F: #v::ViewTag> ::core::marker::Copy for #view<'__v, #used_lifetimes #used __F> {}
        impl<'__v, #lifetimes #declared __F: #v::ViewTag> #view<'__v, #used_lifetimes #used __F> {
            /// The view of the record a cursor finds; does not validate.
            #[doc(hidden)]
            #[inline]
            pub fn __trusted_at(cursor: #v::Cursor<'__v, __F>) -> Self {
                Self { cursor, _type: #d::PhantomData }
            }
            /// Where the record is.
            #[doc(hidden)]
            #[inline]
            pub fn __cursor(&self) -> #v::Cursor<'__v, __F> {
                self.cursor
            }
        }
        impl<#lifetimes #declared> #v::ViewShape for #this
        where
            #(#predicates,)*
            #(#shape_bounds,)*
        {
            type Members = #members_list;
            type Shape<'__v, __F: #v::ViewTag, __R: #v::FieldList<__F, Self::Members>> = #view<'__v, #used_lifetimes #used __F>;
        }
    });

    // The members.
    let fields_of = quote!(<__F as #v::Reads<#this>>::Fields);
    let mut methods = Vec::new();
    for (index, m) in list.iter().enumerate() {
        let (method, mvis, ty) = (&m.method, m.vis, &m.ty);
        let doc = format!("The member `{}`.", method);
        let marker = quote!(#at<#used_lifetimes #used #index>);
        let reader = quote!(<<#marker as #d::ReaderAt<__F, #fields_of>>::Reader as #v::FieldList<__F, <#marker as #d::ListAt>::L>>);
        let shape = quote!(#v::View<'__v, #ty, #reader::Beneath>);
        methods.push(quote! {
            #[doc = #doc]
            #[inline]
            #mvis fn #method(&self) -> #shape {
                #reader::trusted_head(self.cursor, 0)
            }
        });
    }
    out.extend(quote! {
        impl<'__v, #lifetimes #declared __F: #v::Reads<#this>> #view<'__v, #used_lifetimes #used __F>
        where
            #(#predicates,)*
            #(#shape_bounds,)*
        {
            /// Validates `bytes` as one record, then borrows them.
            #[inline]
            pub fn new(bytes: &'__v [u8]) -> ::core::result::Result<Self, <__F as #v::ViewTag>::Error> {
                #v::read::<#this, __F>(bytes)
            }
            /// A view of bytes the caller vouches for (validated earlier,
            /// or produced by the format's encoder); does not validate.
            #[inline]
            pub fn trusted(bytes: &'__v [u8]) -> Self {
                #v::read_trusted::<#this, __F>(bytes)
            }
            /// The owned value.
            #[inline]
            pub fn unpack(&self) -> #this
            where
                __F: #v::Unpacks<#this>,
            {
                <__F as #v::Unpacks<#this>>::trusted_unpack(self.cursor)
            }
            #(#methods)*
        }
        /// Prints the value the view is of.
        impl<'__v, #lifetimes #declared __F: #v::Reads<#this> + #v::Unpacks<#this>> ::core::fmt::Debug for #view<'__v, #used_lifetimes #used __F>
        where
            #this: ::core::fmt::Debug,
            #(#predicates,)*
            #(#shape_bounds,)*
        {
            fn fmt(&self, f: &mut ::core::fmt::Formatter<'_>) -> ::core::fmt::Result {
                ::core::fmt::Debug::fmt(&self.unpack(), f)
            }
        }
    });

    if said.pssz {
        let fields_ty = format_ident!("__{}ViewPssz", name);
        let family = quote!(#v::pssz::PsszFamily);
        // With parameters, what PSSZ needs of the record and of its
        // members is required.
        let mut pssz_bounds = Vec::new();
        if p.generic {
            pssz_bounds.push(quote!(#this: #k::pssz::PsszPack<__F> + #k::pssz::PsszValidate<__F>));
            for (index, ty) in types.iter().enumerate() {
                pssz_bounds.push(quote!(#this: #k::pssz_view::PsszField<__F, #index, Field = #ty>));
                pssz_bounds.push(quote!(#ty: #v::ViewIn<#family, <#this as #k::pssz_view::PsszField<__F, #index>>::Beneath>));
            }
        }
        out.extend(quote! {
            #[doc(hidden)]
            #[allow(non_camel_case_types)]
            #vis struct #fields_ty<#lifetimes #declared __F, const __I: usize>(#d::PhantomData<(fn() -> #this, __F)>);
            impl<#lifetimes #declared __F: #k::pssz::PsszFormat> #v::ViewIn<#family, __F> for #this
            where
                #(#predicates,)*
                #(#shape_bounds,)*
                #(#pssz_bounds,)*
            {
                type Fields = #fields_ty<#used_lifetimes #used __F, 0usize>;
                #[inline]
                fn validate(bytes: &[u8]) -> #k::pssz::PsszResult<()> {
                    #d::pssz_validate::<#this, __F>(bytes)
                }
                #[inline]
                fn trusted_root<'__v>(bytes: &'__v [u8]) -> #view<'__v, #used_lifetimes #used __F> {
                    #view::__trusted_at(#d::pssz_trusted_cursor(bytes))
                }
            }
        });
        for (index, ty) in types.iter().enumerate() {
            let beneath = quote!(<#this as #k::pssz_view::PsszField<__F, #index>>::Beneath);
            let next = index + 1;
            let tail = if next < types.len() {
                quote!(#fields_ty<#used_lifetimes #used __F, #next>)
            } else {
                quote!(#v::Nil<__F, #beneath>)
            };
            out.extend(quote! {
                impl<#lifetimes #declared __F: #k::pssz::PsszFormat>
                    #v::FieldList<__F, <#at<#used_lifetimes #used #index> as #d::ListAt>::L>
                    for #fields_ty<#used_lifetimes #used __F, #index>
                where
                    #(#predicates,)*
                    #(#shape_bounds,)*
                    #(#pssz_bounds,)*
                {
                    type Beneath = #beneath;
                    type Tail = #tail;
                    #[inline]
                    fn trusted_head<'__v>(record: #v::Cursor<'__v, __F>, _: usize) -> #v::View<'__v, #ty, #beneath> {
                        #d::pssz_trusted_field::<#this, __F, #ty, #index>(record)
                    }
                }
            });
        }
    }

    if said.flatbuf {
        let fields_ty = format_ident!("__{}ViewFb", name);
        let tag = quote!(#k::flatbuf::Flatbuf);
        let family = quote!(#v::flatbuf::FbFamily);
        out.extend(quote! {
            #[doc(hidden)]
            #[allow(non_camel_case_types)]
            #vis struct #fields_ty<const __I: usize>;
            impl #v::ViewIn<#family, #tag> for #name {
                type Fields = #fields_ty<0usize>;
                #[inline]
                fn validate(bytes: &[u8]) -> ::core::result::Result<(), #k::flatbuf::FbError> {
                    #d::fb_validate::<#name>(bytes)
                }
                #[inline]
                fn trusted_root<'__v>(bytes: &'__v [u8]) -> #view<'__v, #tag> {
                    #view::__trusted_at(#d::fb_trusted_root::<#name>(bytes))
                }
            }
            impl #v::flatbuf::FbShape for #name {
                type Native = #name;
                #[inline]
                fn trusted_from_fb<'__v>(view: #k::flatbuf::FbView<'__v, #name>) -> #view<'__v, #tag>
                where
                    '__v: '__v,
                {
                    #view::__trusted_at(#d::fb_trusted_record(view))
                }
            }
            impl #v::UnpackIn<#family, #tag> for #name {
                #[inline]
                fn trusted_unpack(record: #v::Cursor<'_, #tag>) -> #name {
                    #d::fb_trusted_unpack::<#name>(record)
                }
            }
        });
        for (index, m) in list.iter().enumerate() {
            let (ty, native) = (&m.ty, m.native.expect("named members"));
            let next = index + 1;
            let tail = if next < list.len() { quote!(#fields_ty<#next>) } else { quote!(#v::Nil<#tag, #tag>) };
            out.extend(quote! {
                impl #v::FieldList<#tag, <#at<#index> as #d::ListAt>::L> for #fields_ty<#index> {
                    type Beneath = #tag;
                    type Tail = #tail;
                    #[inline]
                    fn trusted_head<'__v>(record: #v::Cursor<'__v, #tag>, _: usize) -> #v::View<'__v, #ty, #tag> {
                        <#ty as #v::flatbuf::FbShape>::trusted_from_fb(
                            <#k::flatbuf::FbView<'__v, #name> as #fb_accessors<'__v>>::#native(
                                &#d::fb_trusted_native::<#name>(record),
                            ),
                        )
                    }
                }
            });
        }
    }
    Ok(out)
}

// ── Enums without payloads ──────────────────────────────────────────────────

fn value_enum(input: &DeriveInput, k: &syn::Path, said: &Said) -> syn::Result<Tokens> {
    let name = &input.ident;
    let v = quote!(#k::view);
    if !input.generics.params.is_empty() {
        return Err(syn::Error::new_spanned(&input.generics, "psio View: an enum with generic parameters is not supported"));
    }
    let mut out = quote! {
        impl #v::ViewShape for #name {
            type Members = ();
            type Shape<'__v, __F: #v::ViewTag, __R: #v::FieldList<__F, ()>> = #name;
        }
    };
    if said.pssz {
        let message = format!("psio view: not a value of {name}; validate first");
        out.extend(quote! {
            impl<__F: #k::pssz::PsszFormat> #v::ViewIn<#v::pssz::PsszFamily, __F> for #name {
                type Fields = #v::Nil<__F, __F>;
                #[inline]
                fn validate(bytes: &[u8]) -> #k::pssz::PsszResult<()> {
                    #v::__derive::pssz_validate::<#name, __F>(bytes)
                }
                /// Panics on bytes that are not a value of the enum, which
                /// only a view of bytes nobody validated can hold.
                #[inline]
                fn trusted_root<'__v>(bytes: &'__v [u8]) -> #name {
                    #k::pssz::decode::<__F, #name>(bytes).expect(#message)
                }
            }
        });
    }
    if said.flatbuf {
        let tag = quote!(#k::flatbuf::Flatbuf);
        out.extend(quote! {
            impl #v::ViewIn<#v::flatbuf::FbFamily, #tag> for #name {
                type Fields = #v::Nil<#tag, #tag>;
                fn validate(_: &[u8]) -> ::core::result::Result<(), #k::flatbuf::FbError> {
                    ::core::result::Result::Err(#v::__derive::fb_not_a_root())
                }
                #[inline]
                fn trusted_root<'__v>(bytes: &'__v [u8]) -> #name {
                    <#name as #k::flatbuf::FbViewable>::fb_view_absent(bytes, ::core::option::Option::None)
                }
            }
            impl #v::flatbuf::FbShape for #name {
                type Native = #name;
                #[inline]
                fn trusted_from_fb<'__v>(view: #name) -> #name
                where
                    '__v: '__v,
                {
                    view
                }
            }
        });
    }
    Ok(out)
}

// ── Enums with payloads ─────────────────────────────────────────────────────

fn variants(input: &DeriveInput, data: &syn::DataEnum, k: &syn::Path, said: &Said) -> syn::Result<Tokens> {
    let name = &input.ident;
    let vis = &input.vis;
    let v = quote!(#k::view);
    let d = quote!(#k::view::__derive);
    if !input.generics.params.is_empty() {
        return Err(syn::Error::new_spanned(&input.generics, "psio View: an enum with generic parameters is not supported"));
    }
    let p = params(input);
    let this = quote!(#name);
    let view = said.name.clone().unwrap_or_else(|| format_ident!("{}View", name));
    let at = format_ident!("__{}ViewAt", name);

    // An alternative: its name and what it holds; `first` is the position
    // of its first payload among the payloads of all the alternatives.
    struct Alternative<'a> {
        ident: &'a syn::Ident,
        holds: Vec<&'a Type>,
        first: usize,
    }
    let mut alternatives = Vec::new();
    let mut types: Vec<Type> = Vec::new();
    for variant in &data.variants {
        let mut holds = Vec::new();
        match &variant.fields {
            Fields::Unit => {}
            Fields::Unnamed(fields) => {
                for field in &fields.unnamed {
                    if attrs::annotated(field, &attrs::field(field)?, k)?.is_some() {
                        return Err(syn::Error::new_spanned(field, "psio View: a bound on the payload of a variant is not supported"));
                    }
                    holds.push(&field.ty);
                }
            }
            Fields::Named(_) => {
                return Err(syn::Error::new_spanned(
                    variant,
                    "psio View: a variant with named fields is not supported; hold a record: `Name(Record)`",
                ))
            }
        }
        alternatives.push(Alternative { ident: &variant.ident, holds: holds.clone(), first: types.len() });
        types.extend(holds.into_iter().cloned());
    }

    let mut out = markers(k, vis, &at, &p, &this, &types, &[]);
    let members_list = quote!(<#at<0usize> as #d::ListAt>::L);
    // What payload `index` reads as, when the readers of the payloads are `__R`.
    let reader = |index: usize| {
        quote!(<<#at<#index> as #d::ReaderAt<__F, __R>>::Reader as #v::FieldList<__F, <#at<#index> as #d::ListAt>::L>>)
    };
    let shape = |index: usize| {
        let (ty, reader) = (&types[index], reader(index));
        quote!(#v::View<'__v, #ty, #reader::Beneath>)
    };
    let names = |a: &Alternative| -> Vec<syn::Ident> { (0..a.holds.len()).map(|i| format_ident!("__{}", i)).collect() };

    let arms = alternatives.iter().map(|a| {
        let ident = a.ident;
        let shapes: Vec<_> = (0..a.holds.len()).map(|i| shape(a.first + i)).collect();
        if shapes.is_empty() {
            quote!(#ident)
        } else {
            quote!(#ident(#(#shapes),*))
        }
    });
    let all_shapes: Vec<_> = (0..types.len()).map(shape).collect();
    let debugs = alternatives.iter().map(|a| {
        let (ident, names) = (a.ident, names(a));
        let text = ident.to_string();
        if names.is_empty() {
            quote!(Self::#ident => f.write_str(#text))
        } else {
            quote!(Self::#ident(#(#names),*) => f.debug_tuple(#text)#(.field(#names))*.finish())
        }
    });
    // (The list is written out here: a bound on a type parameter that
    // names the list through a projection is not used by the compiler to
    // prove the same bound with the list written out.)
    let written = types.iter().rev().fold(quote!(()), |tail, ty| quote!((#ty, #tail)));
    let generics = quote!(<'__v, __F: #v::ViewTag, __R: #v::FieldList<__F, #written>>);
    let doc = format!(
        "What a [`{name}`] reads as, in any format (`View<'_, {name}, F>`): the alternative it holds, with what \
         its payload reads as. To `match` on."
    );
    out.extend(quote! {
        #[doc = #doc]
        #vis enum #view #generics {
            #(#arms,)*
            #[doc(hidden)]
            __Never(::core::convert::Infallible, #d::PhantomData<(&'__v (), fn() -> (__F, __R))>),
        }
        impl #generics ::core::clone::Clone for #view<'__v, __F, __R> {
            fn clone(&self) -> Self {
                *self
            }
        }
        // (Every shape is `Copy`: `ViewShape::Shape` says so.)
        impl #generics ::core::marker::Copy for #view<'__v, __F, __R> {}
        impl #v::ViewShape for #name {
            type Members = #members_list;
            type Shape<'__v, __F: #v::ViewTag, __R: #v::FieldList<__F, Self::Members>> = #view<'__v, __F, __R>;
        }
    });

    // Printing an alternative prints its payload, so the payloads' shapes
    // have to be `Debug`. For an enum that holds itself that requirement
    // never ends (the shape of the payload is this enum's view again, at
    // the payload's format), so such an enum's view is not `Debug`:
    // `#[view(no_debug)]` says the same for two enums that hold each other.
    let holds_itself = types.iter().any(|ty| mentions(quote!(#ty), name));
    if !said.no_debug && !holds_itself {
        out.extend(quote! {
            impl #generics ::core::fmt::Debug for #view<'__v, __F, __R>
        where
            #(#all_shapes: ::core::fmt::Debug,)*
        {
            fn fmt(&self, f: &mut ::core::fmt::Formatter<'_>) -> ::core::fmt::Result {
                match self {
                    #(#debugs,)*
                    Self::__Never(never, _) => match *never {},
                }
            }
        }
        });
    }

    if said.pssz {
        if alternatives.is_empty() || alternatives.len() > 256 {
            return Err(syn::Error::new_spanned(data.enum_token, "PSSZ enums require 1..=256 variants"));
        }
        let fields_ty = format_ident!("__{}ViewPssz", name);
        let family = quote!(#v::pssz::PsszFamily);
        let text = name.to_string();
        let message = format!("psio view: not an alternative of {name}; validate first");
        let head = |index: usize| quote!(<#fields_ty<__F, #index> as #v::FieldList<__F, <#at<#index> as #d::ListAt>::L>>::trusted_head(value, 0));
        let reads = alternatives.iter().enumerate().map(|(tag, a)| {
            let (ident, tag) = (a.ident, tag as u8);
            let heads: Vec<_> = (0..a.holds.len()).map(|i| head(a.first + i)).collect();
            if heads.is_empty() {
                quote!(::core::option::Option::Some(#tag) => #view::#ident)
            } else {
                quote!(::core::option::Option::Some(#tag) => #view::#ident(#(#heads),*))
            }
        });
        let fields0 = if types.is_empty() { quote!(#v::Nil<__F, __F>) } else { quote!(#fields_ty<__F, 0usize>) };
        out.extend(quote! {
            #[doc(hidden)]
            #[allow(non_camel_case_types)]
            #vis struct #fields_ty<__F, const __I: usize>(#d::PhantomData<__F>);
            impl<__F: #k::pssz::PsszFormat> #v::ViewIn<#family, __F> for #name {
                type Fields = #fields0;
                #[inline]
                fn validate(bytes: &[u8]) -> #k::pssz::PsszResult<()> {
                    #d::pssz_validate::<#name, __F>(bytes)
                }
                /// Panics on a selector that is no alternative, which only a
                /// view of bytes nobody validated can hold.
                #[inline]
                fn trusted_root<'__v>(bytes: &'__v [u8]) -> #view<'__v, __F, #fields0> {
                    let value = #d::pssz_trusted_cursor::<__F>(bytes);
                    match #d::pssz_selector(value) {
                        #(#reads,)*
                        _ => panic!(#message),
                    }
                }
            }
        });
        for (tag, a) in alternatives.iter().enumerate() {
            let tag = tag as u8;
            let holds = &a.holds;
            for (position, ty) in a.holds.iter().enumerate() {
                let index = a.first + position;
                let next = index + 1;
                let tail = if next < types.len() { quote!(#fields_ty<__F, #next>) } else { quote!(#v::Nil<__F, __F>) };
                // The payload is the one value, or the tuple of several.
                let read = if a.holds.len() == 1 {
                    quote!(<#ty as #v::ViewIn<#family, __F>>::trusted_root(#d::pssz_bytes(payload)))
                } else {
                    quote!(#d::pssz_trusted_field::<(#(#holds,)*), __F, #ty, #position>(payload))
                };
                out.extend(quote! {
                    impl<__F: #k::pssz::PsszFormat> #v::FieldList<__F, <#at<#index> as #d::ListAt>::L> for #fields_ty<__F, #index> {
                        type Beneath = __F;
                        type Tail = #tail;
                        /// Panics when the enum holds another alternative.
                        #[inline]
                        fn trusted_head<'__v>(value: #v::Cursor<'__v, __F>, _: usize) -> #v::View<'__v, #ty, __F> {
                            let payload = #d::pssz_trusted_payload(value, #tag, #text);
                            #read
                        }
                    }
                });
            }
        }
    }

    if said.flatbuf {
        // A FlatBuffers union: every alternative holds one table; only the
        // first variant may hold nothing (the union's NONE).
        let tag = quote!(#k::flatbuf::Flatbuf);
        let family = quote!(#v::flatbuf::FbFamily);
        let fields_ty = format_ident!("__{}ViewFb", name);
        let none = alternatives.first().filter(|a| a.holds.is_empty()).map(|a| a.ident);
        let tables: Vec<_> = alternatives.iter().filter(|a| a.holds.len() == 1).collect();
        if tables.len() + none.is_some() as usize != alternatives.len() || tables.is_empty() {
            return Err(syn::Error::new_spanned(
                &input.ident,
                "psio View: a FlatBuffers union alternative holds one table (`Name(Table)`), and only the first \
                 variant may hold nothing; leave `flatbuf` out of #[view(...)] otherwise",
            ));
        }
        let head = |index: usize| quote!(<#fields_ty<#index> as #v::FieldList<#tag, <#at<#index> as #d::ListAt>::L>>::trusted_head(value, 0));
        let reads = tables.iter().enumerate().map(|(index, a)| {
            let (ident, wire, head) = (a.ident, index + 1, head(a.first));
            quote!(#wire => #view::#ident(#head))
        });
        let absent = match none {
            Some(none) => quote!(#view::#none),
            None => {
                // As the union's value when absent: its first alternative,
                // every member at its default.
                let (ident, head) = (tables[0].ident, head(tables[0].first));
                quote!(#view::#ident(#head))
            }
        };
        out.extend(quote! {
            #[doc(hidden)]
            #[allow(non_camel_case_types)]
            #vis struct #fields_ty<const __I: usize>;
            impl #v::ViewIn<#family, #tag> for #name {
                type Fields = #fields_ty<0usize>;
                fn validate(_: &[u8]) -> ::core::result::Result<(), #k::flatbuf::FbError> {
                    ::core::result::Result::Err(#d::fb_not_a_root())
                }
                #[inline]
                fn trusted_root<'__v>(bytes: &'__v [u8]) -> #view<'__v, #tag, #fields_ty<0usize>> {
                    <#name as #v::flatbuf::FbShape>::trusted_from_fb(#k::flatbuf::FbUnion::none(bytes))
                }
            }
            impl #v::flatbuf::FbShape for #name {
                type Native = #name;
                #[inline]
                fn trusted_from_fb<'__v>(view: #k::flatbuf::FbUnion<'__v, #name>) -> #view<'__v, #tag, #fields_ty<0usize>>
                where
                    '__v: '__v,
                {
                    let value = #d::fb_trusted_union(view);
                    match #d::fb_union_tag(value) {
                        #(#reads,)*
                        _ => #absent,
                    }
                }
            }
        });
        for (wire, a) in tables.iter().enumerate() {
            let (index, ty, wire) = (a.first, a.holds[0], wire + 1);
            let next = index + 1;
            let tail = if next < types.len() { quote!(#fields_ty<#next>) } else { quote!(#v::Nil<#tag, #tag>) };
            out.extend(quote! {
                impl #v::FieldList<#tag, <#at<#index> as #d::ListAt>::L> for #fields_ty<#index> {
                    type Beneath = #tag;
                    type Tail = #tail;
                    /// When the union holds another alternative: an absent
                    /// table, every member at its default.
                    #[inline]
                    fn trusted_head<'__v>(value: #v::Cursor<'__v, #tag>, _: usize) -> #v::View<'__v, #ty, #tag> {
                        <#ty as #v::flatbuf::FbShape>::trusted_from_fb(#d::fb_trusted_alternative::<#ty>(value, #wire))
                    }
                }
            });
        }
    }
    Ok(out)
}
