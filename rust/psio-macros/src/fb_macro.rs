//! The FlatBuffers derives: `FbPack`, `FbUnpack`, `FbView`.
//!
//! The mapping (psio `flatbuf::field`, C++ `psio/flatbuf.hpp`) is decided by
//! the constants of the member types, which a derive cannot see; so the
//! code written here asks them. A struct is a FlatBuffers table, unless it
//! is `#[psio(definition_will_not_change)]` and all its members are fixed —
//! then it is a FlatBuffers struct, and the constant `STRUCT` says which.
//!
//! * struct → table or struct; members take vtable slots in ascending order
//!   of their field numbers (`#[psio(field = N)]`, the declaration position
//!   by default); `#[psio(default = ..)]` is a scalar member's default;
//!   `#[psio(deprecated)]` keeps the slot and is neither written nor read;
//! * field-less `#[repr(int)]` enum → an enum of its repr type;
//! * enum whose variants hold one table each → union; a leading unit variant
//!   is `NONE`.

use proc_macro::TokenStream;
use proc_macro2::TokenStream as Tokens;
use quote::{format_ident, quote};
use syn::{parse_macro_input, Data, DeriveInput, Fields};

use crate::attrs::{self, Derive};
use crate::c_enum::{self, CEnum};

pub fn fb_pack_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    expand(&input, Which::Pack).unwrap_or_else(|e| e.to_compile_error()).into()
}

pub fn fb_unpack_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    expand(&input, Which::Unpack).unwrap_or_else(|e| e.to_compile_error()).into()
}

pub fn fb_view_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    expand(&input, Which::View).unwrap_or_else(|e| e.to_compile_error()).into()
}

#[derive(Clone, Copy, PartialEq)]
enum Which {
    Pack,
    Unpack,
    View,
}

impl Which {
    fn name(self) -> &'static str {
        match self {
            Which::Pack => "FbPack",
            Which::Unpack => "FbUnpack",
            Which::View => "FbView",
        }
    }
}

fn expand(input: &DeriveInput, which: Which) -> syn::Result<Tokens> {
    let item = attrs::item(input, Derive::Interchange(which.name()))?;
    let k = item.krate();
    if item.transparent {
        return Err(syn::Error::new_spanned(
            &input.ident,
            format!("derive({}) has no transparent types: FlatBuffers has no wrapper-free record", which.name()),
        ));
    }
    match &input.data {
        Data::Struct(data) => {
            let record = Record::new(input, &data.fields, &item, &k)?;
            match which {
                Which::Pack => record.pack(&k),
                Which::Unpack => record.unpack(&k),
                Which::View => record.view(&k),
            }
        }
        Data::Enum(_) => match c_enum::analyze(input)? {
            Some(shape) => {
                if shape.repr.is_none() {
                    return Err(syn::Error::new_spanned(
                        &input.ident,
                        "a field-less enum is a FlatBuffers enum of its underlying integer: add \
                         #[repr(u8)] (or u16/u32/u64/i8/i16/i32/i64)",
                    ));
                }
                no_generics(input, "an enum")?;
                match which {
                    Which::Pack => enum_pack(input, &shape, &k),
                    Which::Unpack => enum_unpack(input, &shape, &k),
                    Which::View => enum_view(input, &shape, &k),
                }
            }
            None => {
                no_generics(input, "a union")?;
                let union = Union::new(input)?;
                match which {
                    Which::Pack => union.pack(&k),
                    Which::Unpack => union.unpack(&k),
                    Which::View => union.view(&k),
                }
            }
        },
        Data::Union(_) => Err(syn::Error::new_spanned(&input.ident, "FlatBuffers derives apply to structs and enums")),
    }
}

fn no_generics(input: &DeriveInput, what: &str) -> syn::Result<()> {
    if input.generics.params.is_empty() {
        Ok(())
    } else {
        Err(syn::Error::new_spanned(&input.generics, format!("the FlatBuffers derives do not support {what} with generic parameters")))
    }
}

/// The generics of the impl: every type parameter also implements `bound`.
fn bounded(input: &DeriveInput, bound: &Tokens) -> syn::Generics {
    let mut generics = input.generics.clone();
    for param in generics.type_params_mut() {
        param.bounds.push(syn::parse_quote!(#bound));
    }
    generics
}

// ── Records ─────────────────────────────────────────────────────────────────

struct Member<'a> {
    member: syn::Member,
    ty: &'a syn::Type,
    number: u32,
    deprecated: bool,
    default: Option<Tokens>,
    /// `#[psio(max = N, element(...), sorted, ...)]`: what the member
    /// declares about its value.
    annotation: Option<attrs::Annotated>,
}

struct Record<'a> {
    input: &'a DeriveInput,
    members: Vec<Member<'a>>,
    /// Member indices by ascending field number (ties in declaration order).
    rank: Vec<usize>,
    dwnc: bool,
    /// `#[psio(invariant = "path")]`.
    invariant: Option<syn::Path>,
}

impl<'a> Record<'a> {
    fn new(input: &'a DeriveInput, fields: &'a Fields, item: &attrs::Attrs, k: &syn::Path) -> syn::Result<Self> {
        let dwnc = item.definition_will_not_change;
        let mut members = Vec::new();
        for (index, m) in attrs::members(fields)?.into_iter().enumerate() {
            members.push(Member {
                annotation: attrs::annotated(m.field, &m.attrs, k)?,
                member: m.member,
                ty: &m.field.ty,
                // C++ `reflect<T>::field_number<I>`: the 1-based position.
                number: m.attrs.field_number.unwrap_or(index as u32 + 1),
                deprecated: m.attrs.deprecated,
                default: m.attrs.default,
            });
        }
        let mut rank: Vec<usize> = (0..members.len()).collect();
        rank.sort_by_key(|&i| members[i].number);
        Ok(Record { input, members, rank, dwnc, invariant: item.invariant.clone() })
    }

    fn types(&self) -> Vec<&syn::Type> {
        self.members.iter().map(|m| m.ty).collect()
    }

    /// The vtable slot of member `i`: the slots of the members before it in
    /// field-number order.
    fn slot(&self, i: usize, k: &syn::Path) -> Tokens {
        let before = self.rank.iter().take_while(|&&m| m != i).map(|&m| self.members[m].ty);
        quote!((0u16 #(+ <#before as #k::flatbuf::FbPack>::SLOTS)*))
    }

    /// `fb_struct_layout([...])` of the members.
    fn layout(&self, k: &syn::Path) -> Tokens {
        let types = self.types();
        quote! {
            #k::flatbuf::fb_struct_layout([
                #((<#types as #k::flatbuf::FbPack>::FIXED_SIZE, <#types as #k::flatbuf::FbPack>::FIXED_ALIGN)),*
            ])
        }
    }

    /// The byte range of member `i` in the struct, given `__l` (the layout).
    fn range(&self, i: usize, k: &syn::Path) -> Tokens {
        let ty = self.members[i].ty;
        quote!(__l.offset[#i]..__l.offset[#i] + <#ty as #k::flatbuf::FbPack>::FIXED_SIZE)
    }

    fn default_option(&self, i: usize) -> Tokens {
        match &self.members[i].default {
            Some(expr) => quote!(::core::option::Option::Some(#expr)),
            None => quote!(::core::option::Option::None),
        }
    }

    fn pack(&self, k: &syn::Path) -> syn::Result<Tokens> {
        let name = &self.input.ident;
        let p = quote!(#k::flatbuf::FbPack);
        let generics = bounded(self.input, &p);
        let (impl_g, _, where_c) = generics.split_for_impl();
        let (_, type_g, _) = self.input.generics.split_for_impl();
        let types = self.types();
        let count = self.members.len();
        let dwnc = self.dwnc;
        let layout = self.layout(k);

        let fixed_writes = self.members.iter().enumerate().map(|(i, m)| {
            let (member, ty, range) = (&m.member, m.ty, self.range(i, k));
            quote!(<#ty as #p>::fb_write_fixed(&self.#member, &mut __dst[#range]);)
        });

        let mut checks = Vec::new();
        let mut children = Vec::new();
        let mut defaults = Vec::new();
        let mut entries = Vec::new();
        let mut arms = Vec::new();
        for &i in &self.rank {
            let m = &self.members[i];
            let (member, ty) = (&m.member, m.ty);
            checks.push(quote! {
                assert!(<#ty as #p>::SUPPORTED,
                    "psio::flatbuf: a member of this record has a type FlatBuffers cannot hold \
                     (see the mapping in psio::flatbuf::field)");
            });
            match &m.default {
                Some(_) => checks.push(quote! {
                    assert!(<#ty as #p>::SCALAR, "psio::flatbuf: #[psio(default = ..)] applies to a scalar member");
                }),
                None => checks.push(quote! {
                    assert!(!<#ty as #p>::SCALAR || <#ty as #p>::HAS_ZERO,
                        "psio::flatbuf: a member whose enum type has no enumerator 0 declares its \
                         default: #[psio(default = \"Type::Variant\")]");
                }),
            }
            if m.deprecated {
                continue;
            }
            let child = format_ident!("__child_{}", i);
            children.push(quote!(let #child = <#ty as #p>::fb_write_child(&self.#member, __b, false);));
        }
        for (i, m) in self.members.iter().enumerate() {
            let (member, ty, slot) = (&m.member, m.ty, self.slot(i, k));
            let index = i as u16;
            entries.push(quote!(#k::flatbuf::FbAddEntry::of::<#ty>(#index, #slot)));
            if m.deprecated {
                continue;
            }
            let (child, default) = (format_ident!("__child_{}", i), format_ident!("__default_{}", i));
            let value = match &m.default {
                Some(expr) => quote!(::core::option::Option::Some(#expr)),
                None => quote!(<#ty as #p>::fb_zero()),
            };
            defaults.push(quote!(let #default: ::core::option::Option<#ty> = #value;));
            arms.push(quote! {
                #index => <#ty as #p>::fb_add(&self.#member, __b, __e.part, #child, #default.as_ref(), #slot),
            });
        }

        Ok(quote! {
            impl #impl_g #p for #name #type_g #where_c {
                const STRUCT: bool = #dwnc && #count > 0 #(&& <#types as #p>::FIXED)*;
                const TABLE: bool = !<Self as #p>::STRUCT;
                const FIXED_SIZE: usize = if <Self as #p>::STRUCT { #layout.size } else { 0 };
                const FIXED_ALIGN: usize = if <Self as #p>::STRUCT { #layout.align } else { 1 };

                #[allow(unused_variables)]
                fn fb_write_fixed(&self, __dst: &mut [u8]) {
                    let __l = const { #layout };
                    #(#fixed_writes)*
                }

                fn fb_write_child(&self, __b: &mut #k::flatbuf::FbBuilder, _keep_empty: bool) -> u32 {
                    if <Self as #p>::STRUCT { 0 } else { <Self as #p>::fb_write_table(self, __b) }
                }

                #[allow(unused_variables)]
                fn fb_write_table(&self, __b: &mut #k::flatbuf::FbBuilder) -> u32 {
                    const { #(#checks)* };
                    // Children first, in field order.
                    #(#children)*
                    __b.start_table();
                    #(#defaults)*
                    let __order = const { #k::flatbuf::fb_add_order([#(#entries),*]) };
                    for __e in __order.iter().flatten() {
                        if !__e.live {
                            break;
                        }
                        match __e.member {
                            #(#arms)*
                            _ => {}
                        }
                    }
                    __b.end_table()
                }
            }
        })
    }

    fn unpack(&self, k: &syn::Path) -> syn::Result<Tokens> {
        let name = &self.input.ident;
        let p = quote!(#k::flatbuf::FbPack);
        let u = quote!(#k::flatbuf::FbUnpack);
        let generics = bounded(self.input, &u);
        let (impl_g, _, where_c) = generics.split_for_impl();
        let (_, type_g, _) = self.input.generics.split_for_impl();
        let types = self.types();
        let layout = self.layout(k);

        let mut valid = Vec::new();
        let mut fixed = Vec::new();
        let mut absent = Vec::new();
        let mut checks = Vec::new();
        let mut decoded = Vec::new();
        for (i, m) in self.members.iter().enumerate() {
            let (member, ty, range, slot) = (&m.member, m.ty, self.range(i, k), self.slot(i, k));
            let default = self.default_option(i);
            valid.push(quote!(&& <#ty as #u>::fb_fixed_valid(&__src[#range])));
            fixed.push(quote!(#member: <#ty as #u>::fb_read_fixed(&__src[#range])));
            absent.push(quote!(#member: <#ty as #u>::fb_absent()));
            if m.deprecated {
                decoded.push(quote! {
                    #member: {
                        let __default: ::core::option::Option<#ty> = #default;
                        __default.unwrap_or_else(<#ty as #u>::fb_absent)
                    }
                });
            } else {
                checks.push(quote!(&& <#ty as #u>::fb_check_member(__c, &__t, #slot, false)));
                decoded.push(quote!(#member: <#ty as #u>::fb_decode_member(__t, #slot, #default)));
            }
        }

        // What the type declares about its values (psio::contract): the
        // members in declaration order, what each holds before its own
        // declarations, the invariant last. A deprecated member is not read.
        let contract = quote!(#k::contract);
        let mut declares = Vec::new();
        let mut declared = Vec::new();
        for m in self.members.iter().filter(|m| !m.deprecated) {
            let (member, ty) = (&m.member, m.ty);
            declares.push(quote!(__any |= <#ty as #u>::fb_declares(__seen);));
            declared.push(quote!(<#ty as #u>::fb_check_declared(&self.#member)?;));
            if let Some(attrs::Annotated { own, spec }) = &m.annotation {
                declared.push(quote! {
                    #contract::member::<#own, #spec>(&self.#member, "flatbuf: member length_bound violated")?;
                });
            }
        }
        let own = self.invariant.is_some() || self.members.iter().any(|m| !m.deprecated && m.annotation.is_some());
        let invariant = self.invariant.as_ref().map(|path| {
            quote! {
                if let ::core::result::Result::Err(__why) = #path(self) {
                    return ::core::result::Result::Err(#contract::invariant(__why, "flatbuf: invariant violated"));
                }
            }
        });
        // A type without parameters keeps the answer.
        let kept = self.input.generics.params.is_empty().then(|| {
            quote! {
                fn fb_has_declarations() -> bool {
                    static __KEPT: #contract::Declares = #contract::Declares::new();
                    __KEPT.get(|| <Self as #u>::fb_declares(&mut #contract::Seen::new()))
                }
            }
        });

        Ok(quote! {
            impl #impl_g #u for #name #type_g #where_c {
                const FIXED_CHECKED: bool = <Self as #p>::STRUCT && (false #(|| <#types as #u>::FIXED_CHECKED)*);

                fn fb_declares(__seen: &mut #contract::Seen) -> bool {
                    if !__seen.enter::<Self>() {
                        return false;
                    }
                    let mut __any = #own;
                    #(#declares)*
                    __any
                }

                #kept

                fn fb_check_declared(&self) -> ::core::result::Result<(), &'static str> {
                    #(#declared)*
                    #invariant
                    ::core::result::Result::Ok(())
                }

                #[allow(unused_variables)]
                fn fb_fixed_valid(__src: &[u8]) -> bool {
                    let __l = const { #layout };
                    true #(#valid)*
                }

                fn fb_check_cell(__c: &mut #k::flatbuf::FbChecker<'_>, __pos: u64) -> bool {
                    if <Self as #p>::STRUCT {
                        #k::flatbuf::fb_check_fixed::<Self>(__c, __pos)
                    } else {
                        let __target = __c.check_offset_cell(__pos);
                        __target != 0 && <Self as #u>::fb_check_table(__c, __target)
                    }
                }

                fn fb_check_table(__c: &mut #k::flatbuf::FbChecker<'_>, __pos: u64) -> bool {
                    let ::core::option::Option::Some(__t) = __c.begin_table(__pos) else {
                        return false;
                    };
                    let __ok = true #(#checks)*;
                    __c.end_table();
                    __ok
                }

                #[allow(unused_variables)]
                fn fb_read_fixed(__src: &[u8]) -> Self {
                    let __l = const { #layout };
                    Self { #(#fixed),* }
                }

                fn fb_decode_cell(__buf: &[u8], __cell: usize) -> Self {
                    if <Self as #p>::STRUCT {
                        #k::flatbuf::fb_fixed_at::<Self>(__buf, __cell)
                    } else {
                        <Self as #u>::fb_decode_table(#k::flatbuf::FbTable::follow(__buf, __cell))
                    }
                }

                #[allow(unused_variables)]
                fn fb_decode_table(__t: #k::flatbuf::FbTable<'_>) -> Self {
                    Self { #(#decoded),* }
                }

                fn fb_absent() -> Self {
                    if <Self as #p>::STRUCT {
                        Self { #(#absent),* }
                    } else {
                        <Self as #u>::fb_decode_table(#k::flatbuf::FbTable::absent(&[]))
                    }
                }
            }
        })
    }

    fn view(&self, k: &syn::Path) -> syn::Result<Tokens> {
        no_generics(self.input, "a view of a record")?;
        let name = &self.input.ident;
        let vis = &self.input.vis;
        let p = quote!(#k::flatbuf::FbPack);
        let v = quote!(#k::flatbuf::FbViewable);
        let accessors = format_ident!("{}FbAccessors", name);
        let layout = self.layout(k);

        let mut declarations = Vec::new();
        let mut methods = Vec::new();
        for (i, m) in self.members.iter().enumerate() {
            let syn::Member::Named(method) = &m.member else { continue };
            let (ty, slot, default) = (m.ty, self.slot(i, k), self.default_option(i));
            let doc = format!("The member `{}`.", method);
            declarations.push(quote!(#[doc = #doc] fn #method(&self) -> <#ty as #v>::View<'__fb>;));
            let table = if m.deprecated {
                quote!(<#ty as #v>::fb_view_absent(self.fb_buffer(), #default))
            } else {
                quote!(<#ty as #v>::fb_view_member(self.fb_table(), #slot, #default))
            };
            methods.push(quote! {
                fn #method(&self) -> <#ty as #v>::View<'__fb> {
                    if <#name as #p>::STRUCT {
                        let __l = const { #layout };
                        <#ty as #v>::fb_view_fixed(self.fb_buffer(), self.fb_position().saturating_add(__l.offset[#i]))
                    } else {
                        #table
                    }
                }
            });
        }
        let doc = format!("The members of a [`{name}`] read in place: implemented for `FbView<{name}>`.");

        Ok(quote! {
            impl #v for #name {
                type View<'__fb> = #k::flatbuf::FbView<'__fb, #name>;
                fn fb_view_cell(__buf: &[u8], __cell: usize) -> #k::flatbuf::FbView<'_, #name> {
                    if <#name as #p>::STRUCT {
                        #k::flatbuf::FbView::at(__buf, __cell)
                    } else {
                        #k::flatbuf::FbView::at(__buf, #k::flatbuf::FbTable::follow(__buf, __cell).position())
                    }
                }
                fn fb_view_absent(__buf: &[u8], _default: ::core::option::Option<Self>) -> #k::flatbuf::FbView<'_, #name> {
                    #k::flatbuf::FbView::absent(__buf)
                }
            }

            #[doc = #doc]
            #vis trait #accessors<'__fb> {
                #(#declarations)*
            }

            impl<'__fb> #accessors<'__fb> for #k::flatbuf::FbView<'__fb, #name> {
                #(#methods)*
            }
        })
    }
}

// ── Enums ───────────────────────────────────────────────────────────────────

/// `if __v == VALUE { return RESULT(Self::Variant); }` for every declared
/// enumerator, after the statements that define the values.
fn enum_matches(shape: &CEnum<'_>, wrap: impl Fn(Tokens) -> Tokens) -> Tokens {
    let (preamble, values) = shape.values();
    let variants = shape.declared.iter().map(|v| &v.ident);
    let results = variants.map(|v| wrap(quote!(Self::#v)));
    quote! {
        #preamble
        #(if __v == #values { return #results; })*
    }
}

fn enum_pack(input: &DeriveInput, shape: &CEnum<'_>, k: &syn::Path) -> syn::Result<Tokens> {
    let name = &input.ident;
    let repr = shape.repr.as_ref().expect("checked");
    let p = quote!(#k::flatbuf::FbPack);
    let (preamble, values) = shape.values();
    let variants: Vec<_> = shape.declared.iter().map(|v| &v.ident).collect();
    let has_other = shape.other.is_some();
    let other_arm = shape.other.map(|other| {
        let other = &other.ident;
        quote!(Self::#other(__n) => *__n,)
    });
    let zero = enum_matches(shape, |v| quote!(::core::option::Option::Some(#v)));
    let zero_tail = match shape.other {
        Some(other) => {
            let other = &other.ident;
            quote!(::core::option::Option::Some(Self::#other(__v)))
        }
        None => quote!(::core::option::Option::None),
    };
    Ok(quote! {
        impl #p for #name {
            const SCALAR: bool = true;
            const FIXED_SIZE: usize = ::core::mem::size_of::<#repr>();
            const FIXED_ALIGN: usize = ::core::mem::size_of::<#repr>();
            const HAS_ZERO: bool = {
                #preamble
                #has_other #(|| #values == 0)*
            };
            #[allow(unreachable_code)]
            fn fb_zero() -> ::core::option::Option<Self> {
                let __v: #repr = 0;
                #zero
                #zero_tail
            }
            fn fb_write_fixed(&self, __dst: &mut [u8]) {
                #preamble
                let __v: #repr = match self {
                    #(Self::#variants => #values,)*
                    #other_arm
                };
                __dst.copy_from_slice(&__v.to_le_bytes());
            }
        }
    })
}

fn enum_unpack(input: &DeriveInput, shape: &CEnum<'_>, k: &syn::Path) -> syn::Result<Tokens> {
    let name = &input.ident;
    let repr = shape.repr.as_ref().expect("checked");
    let p = quote!(#k::flatbuf::FbPack);
    let u = quote!(#k::flatbuf::FbUnpack);
    let (preamble, values) = shape.values();
    let closed = shape.other.is_none();
    let first = &shape.declared.first().or(shape.other.as_ref()).expect("an enum has a variant").ident;
    let read = enum_matches(shape, |v| v);
    let (read_tail, absent_tail) = match shape.other {
        Some(other) => {
            let other = &other.ident;
            (quote!(Self::#other(__v)), quote!(Self::#other(0)))
        }
        None => (quote!(<Self as #u>::fb_absent()), quote!(Self::#first)),
    };
    Ok(quote! {
        impl #u for #name {
            // Without a #[psio(other)] catch-all only the declared
            // enumerators are values of the type.
            const FIXED_CHECKED: bool = #closed;
            fn fb_fixed_valid(__src: &[u8]) -> bool {
                let __v = <#repr as #u>::fb_read_fixed(__src);
                #preamble
                !#closed #(|| __v == #values)*
            }
            fn fb_check_cell(__c: &mut #k::flatbuf::FbChecker<'_>, __pos: u64) -> bool {
                #k::flatbuf::fb_check_fixed::<Self>(__c, __pos)
            }
            fn fb_key_less(__buf: &[u8], __a: ::core::option::Option<usize>, __b: ::core::option::Option<usize>) -> bool {
                <#repr as #u>::fb_key_less(__buf, __a, __b)
            }
            #[allow(unreachable_code)]
            fn fb_read_fixed(__src: &[u8]) -> Self {
                let __v = <#repr as #u>::fb_read_fixed(__src);
                #read
                #read_tail
            }
            fn fb_decode_cell(__buf: &[u8], __cell: usize) -> Self {
                #k::flatbuf::fb_fixed_at::<Self>(__buf, __cell)
            }
            fn fb_absent() -> Self {
                match <Self as #p>::fb_zero() {
                    ::core::option::Option::Some(__zero) => __zero,
                    ::core::option::Option::None => #absent_tail,
                }
            }
        }
    })
}

fn enum_view(input: &DeriveInput, shape: &CEnum<'_>, k: &syn::Path) -> syn::Result<Tokens> {
    let name = &input.ident;
    let repr = shape.repr.as_ref().expect("checked");
    let p = quote!(#k::flatbuf::FbPack);
    let u = quote!(#k::flatbuf::FbUnpack);
    let v = quote!(#k::flatbuf::FbViewable);
    Ok(quote! {
        impl #v for #name {
            type View<'__fb> = #name;
            fn fb_view_cell(__buf: &[u8], __cell: usize) -> #name {
                #k::flatbuf::fb_fixed_at::<Self>(__buf, __cell)
            }
            fn fb_view_absent(_buf: &[u8], __default: ::core::option::Option<Self>) -> #name {
                __default.unwrap_or_else(<Self as #u>::fb_absent)
            }
        }
        impl #k::flatbuf::FbKey for #name {
            type Lookup = #name;
            fn fb_key_cmp(__buf: &[u8], __cell: ::core::option::Option<usize>, __key: &#name) -> ::core::cmp::Ordering {
                let mut __bytes = [0u8; ::core::mem::size_of::<#repr>()];
                <#name as #p>::fb_write_fixed(__key, &mut __bytes);
                <#repr as #k::flatbuf::FbKey>::fb_key_cmp(__buf, __cell, &<#repr>::from_le_bytes(__bytes))
            }
        }
    })
}

// ── Unions ──────────────────────────────────────────────────────────────────

struct Union<'a> {
    input: &'a DeriveInput,
    /// A leading unit variant: the union's `NONE`.
    none: Option<&'a syn::Ident>,
    /// The alternatives that hold a table, with their wire types 1, 2, ….
    alternatives: Vec<(&'a syn::Ident, &'a syn::Type, u8)>,
}

impl<'a> Union<'a> {
    fn new(input: &'a DeriveInput) -> syn::Result<Self> {
        let Data::Enum(data) = &input.data else { unreachable!("an enum") };
        let mut none = None;
        let mut alternatives = Vec::new();
        for (index, variant) in data.variants.iter().enumerate() {
            match &variant.fields {
                Fields::Unit if index == 0 => none = Some(&variant.ident),
                Fields::Unnamed(fields) if fields.unnamed.len() == 1 => {
                    if alternatives.len() == 255 {
                        return Err(syn::Error::new_spanned(variant, "a FlatBuffers union has at most 255 alternatives"));
                    }
                    alternatives.push((&variant.ident, &fields.unnamed[0].ty, alternatives.len() as u8 + 1));
                }
                _ => {
                    return Err(syn::Error::new_spanned(
                        variant,
                        "a FlatBuffers union alternative holds one table: `Name(Table)`; only the \
                         first variant may hold nothing (the union's NONE)",
                    ))
                }
            }
        }
        if alternatives.is_empty() {
            return Err(syn::Error::new_spanned(&input.ident, "a FlatBuffers union has an alternative that holds a table"));
        }
        Ok(Union { input, none, alternatives })
    }

    fn parts(&self) -> (Vec<&syn::Ident>, Vec<&syn::Type>, Vec<u8>) {
        (
            self.alternatives.iter().map(|a| a.0).collect(),
            self.alternatives.iter().map(|a| a.1).collect(),
            self.alternatives.iter().map(|a| a.2).collect(),
        )
    }

    fn pack(&self, k: &syn::Path) -> syn::Result<Tokens> {
        let name = &self.input.ident;
        let p = quote!(#k::flatbuf::FbPack);
        let (variants, types, tags) = self.parts();
        let none_child = self.none.map(|none| quote!(Self::#none => 0,));
        let none_tag = self.none.map(|none| quote!(Self::#none => 0u8,));
        Ok(quote! {
            impl #p for #name {
                const UNION: bool = true;
                // An alternative is a table.
                const SUPPORTED: bool = true #(&& <#types as #p>::TABLE)*;
                fn fb_write_child(&self, __b: &mut #k::flatbuf::FbBuilder, _keep_empty: bool) -> u32 {
                    match self {
                        #none_child
                        #(Self::#variants(__alt) => <#types as #p>::fb_write_table(__alt, __b),)*
                    }
                }
                fn fb_add(
                    &self,
                    __b: &mut #k::flatbuf::FbBuilder,
                    __part: u8,
                    __child: u32,
                    _default: ::core::option::Option<&Self>,
                    __slot: u16,
                ) {
                    if __part == 0 {
                        let __tag = match self {
                            #none_tag
                            #(Self::#variants(_) => #tags,)*
                        };
                        if __tag != 0 {
                            __b.add_inline(4 + 2 * __slot, 1, 1)[0] = __tag;
                        }
                    } else if __child != 0 {
                        __b.add_offset_field(4 + 2 * (__slot + 1), __child);
                    }
                }
            }
        })
    }

    /// The value of a union that is `NONE` or absent: the `NONE` variant,
    /// else the first alternative of defaults.
    fn absent(&self, k: &syn::Path) -> Tokens {
        match self.none {
            Some(none) => quote!(Self::#none),
            None => {
                let (variant, ty, _) = self.alternatives[0];
                quote!(Self::#variant(<#ty as #k::flatbuf::FbUnpack>::fb_absent()))
            }
        }
    }

    fn unpack(&self, k: &syn::Path) -> syn::Result<Tokens> {
        let name = &self.input.ident;
        let u = quote!(#k::flatbuf::FbUnpack);
        let (variants, types, tags) = self.parts();
        let count = self.alternatives.len() as u8;
        let absent = self.absent(k);
        Ok(quote! {
            impl #u for #name {
                fn fb_check_cell(__c: &mut #k::flatbuf::FbChecker<'_>, __pos: u64) -> bool {
                    __c.fail("flatbuf: a union is a member of a table", __pos)
                }
                fn fb_check_member(
                    __c: &mut #k::flatbuf::FbChecker<'_>,
                    __t: &#k::flatbuf::FbCheckedTable,
                    __slot: u16,
                    _required: bool,
                ) -> bool {
                    let ::core::result::Result::Ok(__tc) = __c.checked_field(__t, __slot, 1) else {
                        return false;
                    };
                    let ::core::result::Result::Ok(__vc) = __c.checked_field(__t, __slot + 1, 4) else {
                        return false;
                    };
                    let __type = if __tc != 0 { __c.byte(__tc) } else { 0 };
                    if __type == 0 {
                        return __vc == 0 || __c.fail("flatbuf: union is NONE but has a value", __vc);
                    }
                    if __type > #count {
                        return __c.fail("flatbuf: unknown union type", __tc);
                    }
                    if __vc == 0 {
                        return __c.fail("flatbuf: union has a type but no value", __tc);
                    }
                    let __target = __c.check_offset(__vc);
                    if __target == 0 {
                        return false;
                    }
                    match __type {
                        #(#tags => <#types as #u>::fb_check_table(__c, __target),)*
                        _ => true,
                    }
                }
                fn fb_decode_cell(_buf: &[u8], _cell: usize) -> Self {
                    <Self as #u>::fb_absent()
                }
                fn fb_decode_member(
                    __t: #k::flatbuf::FbTable<'_>,
                    __slot: u16,
                    _default: ::core::option::Option<Self>,
                ) -> Self {
                    let __union = #k::flatbuf::FbUnion::<Self>::member(__t, __slot);
                    match __union.tag() {
                        #(#tags => Self::#variants(<#types as #u>::fb_decode_table(__union.fb_table())),)*
                        _ => <Self as #u>::fb_absent(),
                    }
                }
                fn fb_absent() -> Self {
                    #absent
                }
                fn fb_declares(__seen: &mut #k::contract::Seen) -> bool {
                    if !__seen.enter::<Self>() {
                        return false;
                    }
                    let mut __any = false;
                    #(__any |= <#types as #u>::fb_declares(__seen);)*
                    __any
                }
                fn fb_has_declarations() -> bool {
                    static __KEPT: #k::contract::Declares = #k::contract::Declares::new();
                    __KEPT.get(|| <Self as #u>::fb_declares(&mut #k::contract::Seen::new()))
                }
                #[allow(unreachable_patterns)]
                fn fb_check_declared(&self) -> ::core::result::Result<(), &'static str> {
                    match self {
                        #(Self::#variants(__alt) => <#types as #u>::fb_check_declared(__alt),)*
                        _ => ::core::result::Result::Ok(()),
                    }
                }
            }
        })
    }

    fn view(&self, k: &syn::Path) -> syn::Result<Tokens> {
        let name = &self.input.ident;
        let vis = &self.input.vis;
        let u = quote!(#k::flatbuf::FbUnpack);
        let v = quote!(#k::flatbuf::FbViewable);
        let accessors = format_ident!("{}FbAccessors", name);
        let (variants, types, tags) = self.parts();
        let methods: Vec<_> = variants
            .iter()
            .map(|variant| format_ident!("{}", attrs::Case::Snake.variant(&variant.to_string())))
            .collect();
        let docs = variants.iter().map(|variant| format!("The alternative `{variant}`, when the union holds it."));
        let doc = format!("The alternatives of a [`{name}`] read in place: implemented for `FbUnion<{name}>`.");
        Ok(quote! {
            impl #v for #name {
                type View<'__fb> = #k::flatbuf::FbUnion<'__fb, #name>;
                fn fb_view_cell(__buf: &[u8], _cell: usize) -> #k::flatbuf::FbUnion<'_, #name> {
                    #k::flatbuf::FbUnion::none(__buf)
                }
                fn fb_view_absent(__buf: &[u8], _default: ::core::option::Option<Self>) -> #k::flatbuf::FbUnion<'_, #name> {
                    #k::flatbuf::FbUnion::none(__buf)
                }
                fn fb_view_member(
                    __t: #k::flatbuf::FbTable<'_>,
                    __slot: u16,
                    _default: ::core::option::Option<Self>,
                ) -> #k::flatbuf::FbUnion<'_, #name> {
                    #k::flatbuf::FbUnion::member(__t, __slot)
                }
            }

            #[doc = #doc]
            #vis trait #accessors<'__fb> {
                #(#[doc = #docs] fn #methods(&self) -> ::core::option::Option<#k::flatbuf::FbView<'__fb, #types>>;)*
                /// The value.
                fn unpack(&self) -> #name;
            }

            impl<'__fb> #accessors<'__fb> for #k::flatbuf::FbUnion<'__fb, #name> {
                #(fn #methods(&self) -> ::core::option::Option<#k::flatbuf::FbView<'__fb, #types>> {
                    self.alternative::<#types>(#tags)
                })*
                fn unpack(&self) -> #name {
                    match self.tag() {
                        #(#tags => #name::#variants(<#types as #u>::fb_decode_table(self.fb_table())),)*
                        _ => {
                            // `Self` is the view here.
                            fn __absent() -> #name {
                                <#name as #u>::fb_absent()
                            }
                            __absent()
                        }
                    }
                }
            }
        })
    }
}
