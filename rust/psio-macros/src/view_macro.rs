//! `#[derive(View)]`: the view of a type that has one shape in every format
//! (`psio::view`; gofractally/psio#13).
//!
//! For a record `Order` it writes
//!
//! * `impl ViewShape for Order`: an `Order` reads as `View<'_, Order, F>`;
//! * for each format named in `#[view(...)]` (`pssz`, the default, and
//!   `flatbuf`), that format's traits of `psio::view`, on top of the
//!   format's own view (`#[derive(PsszView)]`, `#[derive(FbView)]`), which
//!   the record must derive too;
//! * `trait OrderFormat`, the bound for code that is generic over the
//!   format: its supertraits are everything reading an `Order` needs;
//! * `trait OrderView`, the members as methods, implemented for
//!   `View<'_, Order, F>` for every `F: OrderFormat`.
//!
//! An enum without payloads reads as its value. An enum with payloads reads
//! as an enum `<Name>View` the derive writes, with one alternative per
//! variant holding what the payload reads as: a view to `match` on.
//!
//! What `OrderFormat` requires of the members' types is read off how they
//! are written: `Vec<T>`, `[T; N]` and `BoundedList` are lists,
//! `BTreeMap` / `BoundedMap` maps, `BTreeSet` / `BoundedSet` sets,
//! `Option<T>` and `Box<T>` are what `T` is, a tuple is its elements, the
//! primitive types and the string types need nothing, and any other type
//! `m::Foo` is taken to derive `View`, with `m::FooFormat` beside it. A
//! member says otherwise with `#[view(value)]` (a value that reads as
//! itself) or `#[view(format = "path::NameFormat")]` (an alias or renamed
//! import of a type that derives `View`).

use crate::attrs;
use crate::c_enum;
use proc_macro::TokenStream;
use proc_macro2::{Span, TokenStream as Tokens};
use quote::{format_ident, quote, quote_spanned};
use syn::spanned::Spanned;
use syn::{parse_macro_input, Data, DeriveInput, Fields, GenericArgument, Lit, Meta, NestedMeta, PathArguments, Type};

pub fn view_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    expand(&input).unwrap_or_else(|e| e.to_compile_error()).into()
}

/// Types that read as a value or a string: nothing is required of the
/// format beneath them.
const LEAVES: [&str; 16] = [
    "bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "String", "RawString",
    "BoundedString", "BoundedRawString", "Uint256",
];

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

/// The formats of `#[view(...)]` on the type: (pssz, flatbuf).
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

/// What a member says about its own type with `#[view(...)]`.
#[derive(Default)]
struct Said {
    /// `#[view(value)]`: the types this derive does not know read as
    /// themselves.
    value: bool,
    /// `#[view(format = "path::NameFormat")]`: the format traits of the
    /// types this derive does not know.
    formats: Vec<syn::Path>,
    /// `#[view(recursive)]`: the types this derive does not know hold this
    /// type in turn; nothing is required of the format for them here.
    recursive: bool,
}

fn said(attrs: &[syn::Attribute]) -> syn::Result<Said> {
    let mut out = Said::default();
    for attr in attrs.iter().filter(|a| a.path.is_ident("view")) {
        let usage = "a member's #[view(...)] takes `value`, `recursive` or `format = \"path::NameFormat\"`";
        let Meta::List(list) = attr.parse_meta()? else {
            return Err(syn::Error::new_spanned(attr, usage));
        };
        for item in &list.nested {
            match item {
                NestedMeta::Meta(Meta::Path(p)) if p.is_ident("value") => out.value = true,
                NestedMeta::Meta(Meta::Path(p)) if p.is_ident("recursive") => out.recursive = true,
                NestedMeta::Meta(Meta::NameValue(nv)) if nv.path.is_ident("format") => match &nv.lit {
                    Lit::Str(s) => out.formats.push(s.parse()?),
                    _ => return Err(syn::Error::new_spanned(nv, usage)),
                },
                other => return Err(syn::Error::new_spanned(other, usage)),
            }
        }
    }
    if (out.value as usize) + (out.recursive as usize) + (!out.formats.is_empty() as usize) > 1 {
        return Err(syn::Error::new(Span::call_site(), "a member's #[view(...)] is one of `value`, `recursive` and `format`"));
    }
    Ok(out)
}

/// What is collected about the types of the members.
struct Needs<'a> {
    k: &'a syn::Path,
    /// The type being derived for.
    this: &'a syn::Ident,
    /// Whether a member holds the type itself.
    holds_itself: bool,
    /// Bounds on the format of the position the members are at.
    bounds: Vec<Tokens>,
    /// Types taken to derive `View`: checked, so that a wrong guess is a
    /// message that says what to write.
    guessed: Vec<Tokens>,
}

fn type_arguments(arguments: &PathArguments) -> Vec<&Type> {
    match arguments {
        PathArguments::AngleBracketed(args) => args
            .args
            .iter()
            .filter_map(|a| match a {
                GenericArgument::Type(ty) => Some(ty),
                _ => None,
            })
            .collect(),
        _ => Vec::new(),
    }
}

impl Needs<'_> {
    /// What the format of a position has to satisfy for a value written
    /// `written` there to be read all the way down. `ty` is the type the
    /// view has for it: `written` itself, except beneath a member with a
    /// bound (`#[psio(max = ...)]`), whose type is found by projection
    /// (`projected`).
    fn of(&mut self, written: &Type, ty: Tokens, projected: bool, said: &Said) -> syn::Result<()> {
        let v = {
            let k = self.k;
            quote!(#k::view)
        };
        let inner = |part: Tokens, written: &Type| if projected { part } else { quote!(#written) };
        match written {
            Type::Paren(group) => self.of(&group.elem, ty, projected, said),
            Type::Group(group) => self.of(&group.elem, ty, projected, said),
            Type::Array(array) => {
                self.bounds.push(quote!(#v::ReadsList<#ty>));
                self.of(&array.elem, inner(quote!(<#ty as #v::ListShape>::Elem), &array.elem), projected, said)
            }
            Type::Tuple(tuple) if !tuple.elems.is_empty() => {
                if projected {
                    return Err(syn::Error::new_spanned(written, "psio View: a tuple beneath a member with a bound is not supported"));
                }
                for elem in &tuple.elems {
                    self.of(elem, quote!(#elem), false, said)?;
                }
                Ok(())
            }
            Type::Path(path) if path.qself.is_none() => {
                let last = path.path.segments.last().expect("a path has a segment");
                let name = last.ident.to_string();
                let args = type_arguments(&last.arguments);
                match (name.as_str(), args.as_slice()) {
                    (leaf, _) if LEAVES.contains(&leaf) => Ok(()),
                    ("Vec" | "BoundedList", [elem]) => {
                        self.bounds.push(quote!(#v::ReadsList<#ty>));
                        self.of(elem, inner(quote!(<#ty as #v::ListShape>::Elem), elem), projected, said)
                    }
                    ("BTreeMap" | "BoundedMap", [key, value]) => {
                        self.bounds.push(quote!(#v::ReadsMap<#ty>));
                        self.of(key, inner(quote!(<#ty as #v::MapShape>::Key), key), projected, said)?;
                        self.of(value, inner(quote!(<#ty as #v::MapShape>::Value), value), projected, said)
                    }
                    ("BTreeSet" | "BoundedSet", [key]) => {
                        self.bounds.push(quote!(#v::ReadsSet<#ty>));
                        self.of(key, inner(quote!(<#ty as #v::SetShape>::Key), key), projected, said)
                    }
                    ("Option" | "Box", [value]) => {
                        self.of(value, inner(quote!(<#ty as #v::WrapShape>::Inner), value), projected, said)
                    }
                    _ if said.value || said.recursive || !said.formats.is_empty() => Ok(()),
                    // The type itself: it is read at the position its
                    // members are at, which the type's own trait says.
                    _ if args.is_empty() && path.path.segments.len() == 1 && (last.ident == *self.this || last.ident == "Self") => {
                        self.holds_itself = true;
                        Ok(())
                    }
                    _ => {
                        // Taken to derive `View`: the trait its derive
                        // wrote beside it.
                        let mut format = path.path.clone();
                        let last = format.segments.last_mut().expect("a path has a segment");
                        last.ident = format_ident!("{}Format", last.ident, span = last.ident.span());
                        last.arguments = PathArguments::None;
                        self.bounds.push(quote!(#format));
                        self.guessed.push(quote_spanned!(written.span()=> #written));
                        Ok(())
                    }
                }
            }
            _ => Err(syn::Error::new_spanned(written, "psio View: unsupported member type")),
        }
    }

    fn member(&mut self, written: &Type, ty: &Type, projected: bool, attrs: &[syn::Attribute]) -> syn::Result<()> {
        let said = said(attrs)?;
        self.of(written, quote!(#ty), projected, &said)?;
        for format in &said.formats {
            self.bounds.push(quote!(#format));
        }
        Ok(())
    }

    /// The checks of the guesses.
    fn checks(&self) -> Tokens {
        let k = self.k;
        let checks = self.guessed.iter().map(|ty| {
            quote_spanned! {ty.span()=>
                const _: fn() = || {
                    fn __psio_view_member<__T: #k::view::Derived + ?Sized>() {}
                    __psio_view_member::<#ty>();
                };
            }
        });
        quote!(#(#checks)*)
    }
}

fn expand(input: &DeriveInput) -> syn::Result<Tokens> {
    let k = krate(input)?;
    let (pssz, flatbuf) = formats(input)?;
    if !input.generics.params.is_empty() {
        return Err(syn::Error::new_spanned(
            &input.generics,
            "psio View: a type with generic parameters is not supported",
        ));
    }
    match &input.data {
        Data::Struct(data) => record(input, &data.fields, &k, pssz, flatbuf),
        Data::Enum(data) => match c_enum::analyze(input)? {
            Some(_) => value_enum(input, &k, pssz, flatbuf),
            None => variants(input, data, &k, pssz, flatbuf),
        },
        Data::Union(data) => Err(syn::Error::new_spanned(data.union_token, "psio View: not for a union")),
    }
}

// ── Records ─────────────────────────────────────────────────────────────────

fn record(input: &DeriveInput, fields: &Fields, k: &syn::Path, pssz: bool, flatbuf: bool) -> syn::Result<Tokens> {
    let name = &input.ident;
    let vis = &input.vis;
    let v = quote!(#k::view);
    let members = attrs::members(fields)?;
    if members.is_empty() {
        return Err(syn::Error::new_spanned(fields, "psio View: a record needs a member"));
    }
    if flatbuf && !matches!(fields, Fields::Named(_)) {
        return Err(syn::Error::new_spanned(
            fields,
            "psio View: the FlatBuffers view of a record has accessors for named members only; \
             name the members or leave `flatbuf` out of #[view(...)]",
        ));
    }
    let format_trait = format_ident!("{}Format", name);
    let view_trait = format_ident!("{}View", name);
    let fb_accessors = format_ident!("{}FbAccessors", name);

    // A member: its accessor, the type as written, and the type the view
    // has for it (the one PSSZ has: for a member with a bound, the bounded
    // type the annotation describes).
    struct Member<'a> {
        method: syn::Ident,
        native: Option<&'a syn::Ident>,
        written: &'a Type,
        ty: Type,
        projected: bool,
        attrs: &'a [syn::Attribute],
    }
    let mut list = Vec::new();
    for m in &members {
        let (method, native) = match &m.member {
            syn::Member::Named(id) => (id.clone(), Some(m.field.ident.as_ref().expect("named"))),
            syn::Member::Unnamed(index) => (format_ident!("field_{}", index.index), None),
        };
        let (ty, projected) = match attrs::annotated(m.field, &m.attrs, k)? {
            Some(annotated) => (annotated.described(k), true),
            None => (m.field.ty.clone(), false),
        };
        list.push(Member { method, native, written: &m.field.ty, ty, projected, attrs: &m.field.attrs });
    }

    let mut out = quote! {
        impl #v::ViewShape for #name {
            type Shape<'__v, __F: #v::ViewTag> = #v::View<'__v, #name, __F>;
        }
        impl #v::Derived for #name {}
    };

    if pssz {
        out.extend(quote! {
            impl<__F: #k::pssz::PsszFormat> #v::pssz::PsszShape<__F> for #name {
                #[inline]
                fn from_pssz<'__v>(bytes: &'__v [u8]) -> #v::View<'__v, #name, __F> {
                    #v::View::at(bytes)
                }
            }
            impl<__F: #k::pssz::PsszFormat> #v::pssz::PsszRecord<__F> for #name {
                type Beneath = <#name as #k::pssz_view::PsszField<__F, 0>>::Beneath;
            }
        });
    }

    if flatbuf {
        let tag = quote!(#k::flatbuf::Flatbuf);
        out.extend(quote! {
            impl #v::flatbuf::FbShape for #name {
                type Native = #name;
                #[inline]
                fn from_fb<'__v>(view: #k::flatbuf::FbView<'__v, #name>) -> #v::View<'__v, #name, #tag>
                where
                    '__v: '__v,
                {
                    #v::flatbuf::record(view)
                }
            }
            impl #v::flatbuf::FbRecord for #name {}
        });
        for (index, m) in list.iter().enumerate() {
            let (ty, native) = (&m.ty, m.native.expect("named members"));
            out.extend(quote! {
                impl #v::flatbuf::FbField<#index> for #name {
                    type Type = #ty;
                    #[inline]
                    fn field<'__v>(record: #v::flatbuf::FbCursor<'__v>) -> <#ty as #v::ViewShape>::Shape<'__v, #tag> {
                        <#ty as #v::flatbuf::FbShape>::from_fb(
                            <#k::flatbuf::FbView<'__v, #name> as #fb_accessors<'__v>>::#native(
                                &#v::flatbuf::native::<#name>(record),
                            ),
                        )
                    }
                }
            });
        }
    }

    // The bound for code generic over the format.
    let mut needs = Needs { k, this: name, holds_itself: false, bounds: Vec::new(), guessed: Vec::new() };
    for m in &list {
        needs.member(m.written, &m.ty, m.projected, m.attrs)?;
    }
    let bounds = &needs.bounds;
    let indices: Vec<usize> = (0..list.len()).collect();
    let types: Vec<_> = list.iter().map(|m| &m.ty).collect();
    let format_doc = format!("A format a [`{name}`] is read in: the bound of code generic over the format.");
    // A record that holds itself is read, inside itself, at the position
    // its members are at; the members of that one are at the same position
    // again. Said as an equality, so that the trait does not name itself.
    let within = needs.holds_itself.then(|| {
        quote! {
            + #v::RecordFormat<#name, Beneath = Self>
            #(+ #v::ReadsField<#name, #indices, Type = #types>)*
        }
    });
    let inner_trait = format_ident!("{}MembersFormat", name);
    let inner_doc = format!("The format of the position the members of a [`{name}`] are at: what [`{format_trait}`] requires of it.");
    out.extend(quote! {
        #[doc = #inner_doc]
        #vis trait #inner_trait: #v::ViewTag #within #(+ #bounds)* {}
        impl<__F> #inner_trait for __F where __F: #v::ViewTag #within #(+ #bounds)* {}

        #[doc = #format_doc]
        #vis trait #format_trait:
            #v::ViewTag
            + #v::RecordFormat<#name, Beneath: #inner_trait>
            #(+ #v::ReadsField<#name, #indices, Type = #types>)*
        {
        }
        impl<__F> #format_trait for __F
        where
            __F: #v::ViewTag + #v::RecordFormat<#name> #(+ #v::ReadsField<#name, #indices, Type = #types>)*,
            <__F as #v::RecordFormat<#name>>::Beneath: #inner_trait,
        {
        }
    });
    out.extend(needs.checks());

    // The members.
    let mut declarations = Vec::new();
    let mut methods = Vec::new();
    for (index, m) in list.iter().enumerate() {
        let (method, ty) = (&m.method, &m.ty);
        let doc = format!("The member `{}`.", method);
        let shape = quote!(<#ty as #v::ViewShape>::Shape<'__v, <__F as #v::RecordFormat<#name>>::Beneath>);
        declarations.push(quote!(#[doc = #doc] fn #method(&self) -> #shape;));
        methods.push(quote! {
            #[inline]
            fn #method(&self) -> #shape {
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

// ── Enums without payloads ──────────────────────────────────────────────────

fn value_enum(input: &DeriveInput, k: &syn::Path, pssz: bool, flatbuf: bool) -> syn::Result<Tokens> {
    let name = &input.ident;
    let vis = &input.vis;
    let v = quote!(#k::view);
    let format_trait = format_ident!("{}Format", name);
    let mut out = quote! {
        impl #v::ViewShape for #name {
            type Shape<'__v, __F: #v::ViewTag> = #name;
        }
        impl #v::Derived for #name {}
    };
    if pssz {
        let message = format!("psio view: not a value of {name}; validate first");
        out.extend(quote! {
            impl<__F: #k::pssz::PsszFormat> #v::pssz::PsszShape<__F> for #name {
                /// Panics on bytes that are not a value of the enum, which
                /// only a view of bytes nobody validated can hold.
                #[inline]
                fn from_pssz<'__v>(bytes: &'__v [u8]) -> #name {
                    #k::pssz::decode::<__F, #name>(bytes).expect(#message)
                }
            }
        });
    }
    if flatbuf {
        out.extend(quote! {
            impl #v::flatbuf::FbShape for #name {
                type Native = #name;
                #[inline]
                fn from_fb<'__v>(view: #name) -> #name
                where
                    '__v: '__v,
                {
                    view
                }
            }
        });
    }
    let format_doc = format!("A format a [`{name}`] is read in. A value: every format reads it.");
    out.extend(quote! {
        #[doc = #format_doc]
        #vis trait #format_trait: #v::ViewTag {}
        impl<__F: #v::ViewTag> #format_trait for __F {}
    });
    Ok(out)
}

// ── Enums with payloads ─────────────────────────────────────────────────────

fn variants(input: &DeriveInput, data: &syn::DataEnum, k: &syn::Path, pssz: bool, flatbuf: bool) -> syn::Result<Tokens> {
    let name = &input.ident;
    let vis = &input.vis;
    let v = quote!(#k::view);
    let format_trait = format_ident!("{}Format", name);
    let view_enum = format_ident!("{}View", name);

    // An alternative: its name and what it holds.
    struct Alternative<'a> {
        ident: &'a syn::Ident,
        holds: Vec<&'a Type>,
    }
    impl Alternative<'_> {
        /// The payload as PSSZ has it: the one value, or the tuple of
        /// several.
        fn payload(&self) -> Option<Type> {
            let holds = &self.holds;
            match holds.as_slice() {
                [] => None,
                [one] => Some((*one).clone()),
                _ => Some(syn::parse_quote!((#(#holds,)*))),
            }
        }
        fn names(&self) -> Vec<syn::Ident> {
            (0..self.holds.len()).map(|i| format_ident!("__{}", i)).collect()
        }
    }
    let mut alternatives = Vec::new();
    let mut needs = Needs { k, this: name, holds_itself: false, bounds: Vec::new(), guessed: Vec::new() };
    for variant in &data.variants {
        let mut holds = Vec::new();
        match &variant.fields {
            Fields::Unit => {}
            Fields::Unnamed(fields) => {
                if fields.unnamed.len() > 4 {
                    return Err(syn::Error::new_spanned(variant, "psio View: a variant holds at most four values"));
                }
                for field in &fields.unnamed {
                    if attrs::annotated(field, &attrs::field(field)?, k)?.is_some() {
                        return Err(syn::Error::new_spanned(field, "psio View: a bound on the payload of a variant is not supported"));
                    }
                    needs.member(&field.ty, &field.ty, false, &field.attrs)?;
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
        alternatives.push(Alternative { ident: &variant.ident, holds });
    }

    let arms = alternatives.iter().map(|a| {
        let (ident, holds) = (a.ident, &a.holds);
        if holds.is_empty() {
            quote!(#ident)
        } else {
            quote!(#ident(#(<#holds as #v::ViewShape>::Shape<'__v, __F>),*))
        }
    });
    let payloads: Vec<_> = alternatives.iter().flat_map(|a| a.holds.iter().copied()).collect();
    let clones = alternatives.iter().map(|a| {
        let (ident, names) = (a.ident, a.names());
        if names.is_empty() {
            quote!(Self::#ident => Self::#ident)
        } else {
            quote!(Self::#ident(#(#names),*) => Self::#ident(#(::core::clone::Clone::clone(#names)),*))
        }
    });
    let debugs = alternatives.iter().map(|a| {
        let (ident, names) = (a.ident, a.names());
        let text = ident.to_string();
        if names.is_empty() {
            quote!(Self::#ident => f.write_str(#text))
        } else {
            quote!(Self::#ident(#(#names),*) => f.debug_tuple(#text)#(.field(#names))*.finish())
        }
    });
    let enum_doc = format!(
        "What a [`{name}`] reads as, in any format: the alternative it holds, with what its payload reads as. \
         To `match` on."
    );
    let mut out = quote! {
        #[doc = #enum_doc]
        #vis enum #view_enum<'__v, __F: #v::ViewTag> {
            #(#arms,)*
            #[doc(hidden)]
            __Never(::core::convert::Infallible, ::core::marker::PhantomData<(&'__v (), fn() -> __F)>),
        }
        impl<'__v, __F: #v::ViewTag> ::core::clone::Clone for #view_enum<'__v, __F>
        where
            #(<#payloads as #v::ViewShape>::Shape<'__v, __F>: ::core::clone::Clone,)*
        {
            fn clone(&self) -> Self {
                match self {
                    #(#clones,)*
                    Self::__Never(never, _) => match *never {},
                }
            }
        }
        impl<'__v, __F: #v::ViewTag> ::core::marker::Copy for #view_enum<'__v, __F>
        where
            #(<#payloads as #v::ViewShape>::Shape<'__v, __F>: ::core::marker::Copy,)*
        {
        }
        impl<'__v, __F: #v::ViewTag> ::core::fmt::Debug for #view_enum<'__v, __F>
        where
            #(<#payloads as #v::ViewShape>::Shape<'__v, __F>: ::core::fmt::Debug,)*
        {
            fn fmt(&self, f: &mut ::core::fmt::Formatter<'_>) -> ::core::fmt::Result {
                match self {
                    #(#debugs,)*
                    Self::__Never(never, _) => match *never {},
                }
            }
        }
        impl #v::ViewShape for #name {
            type Shape<'__v, __F: #v::ViewTag> = #view_enum<'__v, __F>;
        }
        impl #v::Derived for #name {}
    };

    if pssz {
        if alternatives.len() > 256 {
            return Err(syn::Error::new_spanned(data.enum_token, "PSSZ enums require 1..=256 variants"));
        }
        let message = format!("psio view: not an alternative of {name}; validate first");
        let reads = alternatives.iter().enumerate().map(|(index, a)| {
            let (ident, tag, names) = (a.ident, index as u8, a.names());
            match (a.payload(), names.as_slice()) {
                (None, _) => quote!(::core::option::Option::Some((#tag, _)) => #view_enum::#ident),
                (Some(ty), [_]) => quote! {
                    ::core::option::Option::Some((#tag, payload)) =>
                        #view_enum::#ident(<#ty as #v::pssz::PsszShape<__F>>::from_pssz(payload))
                },
                (Some(ty), _) => quote! {
                    ::core::option::Option::Some((#tag, payload)) => {
                        let (#(#names,)*) = <#ty as #v::pssz::PsszShape<__F>>::from_pssz(payload);
                        #view_enum::#ident(#(#names),*)
                    }
                },
            }
        });
        out.extend(quote! {
            impl<__F: #k::pssz::PsszFormat> #v::pssz::PsszShape<__F> for #name {
                /// Panics on a selector that is no alternative, which only a
                /// view of bytes nobody validated can hold.
                #[inline]
                fn from_pssz<'__v>(bytes: &'__v [u8]) -> #view_enum<'__v, __F> {
                    match bytes.split_first().map(|(selector, payload)| (*selector, payload)) {
                        #(#reads,)*
                        _ => panic!(#message),
                    }
                }
            }
        });
    }

    if flatbuf {
        // A FlatBuffers union: every alternative holds one table; only the
        // first variant may hold nothing (the union's NONE).
        let tag = quote!(#k::flatbuf::Flatbuf);
        let none = alternatives.first().filter(|a| a.holds.is_empty()).map(|a| a.ident);
        let tables: Vec<_> = alternatives.iter().filter(|a| a.holds.len() == 1).collect();
        if tables.len() + none.is_some() as usize != alternatives.len() || tables.is_empty() {
            return Err(syn::Error::new_spanned(
                &input.ident,
                "psio View: a FlatBuffers union alternative holds one table (`Name(Table)`), and only the first \
                 variant may hold nothing; leave `flatbuf` out of #[view(...)] otherwise",
            ));
        }
        let reads = tables.iter().enumerate().map(|(index, a)| {
            let (ident, ty, wire) = (a.ident, a.holds[0], index as u8 + 1);
            quote! {
                if let ::core::option::Option::Some(table) = view.alternative::<#ty>(#wire) {
                    return #view_enum::#ident(<#ty as #v::flatbuf::FbShape>::from_fb(table));
                }
            }
        });
        let absent = match none {
            Some(none) => quote!(#view_enum::#none),
            None => {
                // As the union's value when absent: its first alternative,
                // every member at its default.
                let (ident, ty) = (tables[0].ident, tables[0].holds[0]);
                quote! {
                    #view_enum::#ident(<#ty as #v::flatbuf::FbShape>::from_fb(
                        #k::flatbuf::FbView::<#ty>::absent(view.fb_table().buffer()),
                    ))
                }
            }
        };
        out.extend(quote! {
            impl #v::flatbuf::FbShape for #name {
                type Native = #name;
                #[inline]
                fn from_fb<'__v>(view: #k::flatbuf::FbUnion<'__v, #name>) -> #view_enum<'__v, #tag>
                where
                    '__v: '__v,
                {
                    #(#reads)*
                    #absent
                }
            }
        });
    }

    let bounds = &needs.bounds;
    let format_doc = format!("A format a [`{name}`] is read in: the bound of code generic over the format.");
    out.extend(quote! {
        #[doc = #format_doc]
        #vis trait #format_trait: #v::ViewTag #(+ #bounds)* {}
    });
    out.extend(quote!(impl<__F: #v::ViewTag #(+ #bounds)*> #format_trait for __F {}));
    out.extend(needs.checks());
    Ok(out)
}
