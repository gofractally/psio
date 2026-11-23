//! `#[psio::interface]`: a trait's functions as an interface (C++
//! `PSIO_REFLECT(T, method(...))` + `PSIO_INTERFACE(T)`). See
//! `psio::interface` for what is generated.

use proc_macro2::{Span, TokenStream as Tokens};
use quote::quote;
use syn::parse::{Parse, ParseStream};
use syn::punctuated::Punctuated;
use syn::spanned::Spanned;
use syn::{FnArg, Ident, ItemTrait, LitStr, Pat, ReturnType, Token, TraitItem, Type};

/// The arguments of the attribute.
#[derive(Default)]
struct Options {
    name: Option<LitStr>,
    package: Option<LitStr>,
    version: Option<LitStr>,
    module: Option<Ident>,
    krate: Option<syn::Path>,
    types: Vec<Type>,
    derives: Vec<syn::Path>,
}

const KEYS: &str = "name = \"...\", package = \"...\", version = \"...\", module = \"...\", \
                    crate = \"path\", types(A, B), derive(Trait, ...)";

impl Parse for Options {
    fn parse(input: ParseStream) -> syn::Result<Self> {
        let mut options = Options::default();
        while !input.is_empty() {
            // `crate` is a keyword.
            let (key, span) = if input.peek(Token![crate]) {
                let token: Token![crate] = input.parse()?;
                ("crate".to_string(), token.span)
            } else {
                let ident: Ident = input.parse()?;
                (ident.to_string(), ident.span())
            };
            let duplicate = |set: bool| {
                if set {
                    Err(syn::Error::new(span, format!("`{key}` is given twice")))
                } else {
                    Ok(())
                }
            };
            match key.as_str() {
                "name" | "package" | "version" | "module" | "crate" => {
                    input.parse::<Token![=]>()?;
                    let value: LitStr = input.parse()?;
                    match key.as_str() {
                        "name" => {
                            duplicate(options.name.is_some())?;
                            options.name = Some(value);
                        }
                        "package" => {
                            duplicate(options.package.is_some())?;
                            options.package = Some(value);
                        }
                        "version" => {
                            duplicate(options.version.is_some())?;
                            options.version = Some(value);
                        }
                        "module" => {
                            duplicate(options.module.is_some())?;
                            options.module = Some(value.parse()?);
                        }
                        _ => {
                            duplicate(options.krate.is_some())?;
                            options.krate = Some(value.parse()?);
                        }
                    }
                }
                "types" => {
                    let content;
                    syn::parenthesized!(content in input);
                    options.types.extend(Punctuated::<Type, Token![,]>::parse_terminated(&content)?);
                }
                "derive" => {
                    let content;
                    syn::parenthesized!(content in input);
                    options
                        .derives
                        .extend(Punctuated::<syn::Path, Token![,]>::parse_terminated(&content)?);
                }
                _ => {
                    return Err(syn::Error::new(
                        span,
                        format!("unknown key `{key}` in #[psio::interface(...)]; it takes {KEYS}"),
                    ))
                }
            }
            if !input.is_empty() {
                input.parse::<Token![,]>()?;
            }
        }
        if options.version.is_some() && options.package.is_none() {
            return Err(syn::Error::new(Span::call_site(), "`version` is the package's: give `package = \"...\"` too"));
        }
        Ok(options)
    }
}

enum Receiver {
    None,
    Ref,
    Mut,
}

struct Param {
    ident: Ident,
    ty: Type,
    /// `#[psio(...)]` / `#[json(...)]` on the parameter: its member's in
    /// the argument record.
    attrs: Vec<syn::Attribute>,
}

struct Method {
    ident: Ident,
    /// The identifier as text, without `r#`.
    name: String,
    receiver: Receiver,
    params: Vec<Param>,
    /// `None`: the function returns nothing.
    result: Option<Type>,
    /// `#[pssz(max_dynamic_data = N)]`: the cap of the argument record.
    cap: Option<u64>,
    /// `#[psio(max_fields = N)]`: the most parameters the method will ever
    /// have.
    max_fields: Option<u64>,
    /// `#[psio(positional)]`: the parameter names are not part of the
    /// interface (C++ `method(name)` without parameter names).
    positional: bool,
    /// `#[psio(returns(...))]`: the bounds of the result, as the attribute
    /// a member of the result's type would carry.
    returns: Option<syn::Attribute>,
}

/// The bounds of a value of type `ty` under `attrs`, read as a field's.
fn annotation(attrs: &[syn::Attribute], ty: &Type, k: &syn::Path) -> syn::Result<Option<crate::attrs::Annotated>> {
    use syn::parse::Parser;
    let field = syn::Field::parse_named.parse2(quote!(#(#attrs)* value: #ty))?;
    crate::attrs::annotated(&field, &crate::attrs::field(&field)?, k)
}

/// The type a parameter or result has on the wire and in a schema: the
/// type its annotation describes, or its own.
fn wire_type(attrs: &[syn::Attribute], ty: &Type, k: &syn::Path) -> syn::Result<Type> {
    Ok(match annotation(attrs, ty, k)? {
        Some(annotated) => annotated.described(k),
        None => ty.clone(),
    })
}

fn snake_case(name: &str) -> String {
    let mut out = String::new();
    let chars: Vec<char> = name.chars().collect();
    for (i, c) in chars.iter().enumerate() {
        if c.is_uppercase() {
            let after_lower = i > 0 && (chars[i - 1].is_lowercase() || chars[i - 1].is_ascii_digit());
            let before_lower = i > 0 && chars.get(i + 1).is_some_and(|n| n.is_lowercase());
            if after_lower || (before_lower && chars[i - 1] != '_') {
                out.push('_');
            }
            out.extend(c.to_lowercase());
        } else {
            out.push(*c);
        }
    }
    out
}

fn unraw(ident: &Ident) -> String {
    let text = ident.to_string();
    text.strip_prefix("r#").map(str::to_string).unwrap_or(text)
}

/// The annotations of a method.
#[derive(Default)]
struct MethodAnnotations {
    /// `#[pssz(max_dynamic_data = N)]`: the cap of its argument record.
    cap: Option<u64>,
    /// `#[psio(returns(...))]`: the bounds of its result.
    returns: Option<syn::Attribute>,
    /// `#[psio(max_fields = N)]`.
    max_fields: Option<u64>,
    /// `#[psio(positional)]`.
    positional: bool,
}

fn method_annotations(attrs: &[syn::Attribute]) -> syn::Result<MethodAnnotations> {
    const ACCEPTED: &str = "a function of a #[psio::interface] takes #[pssz(max_dynamic_data = N)], \
                            #[psio(max_fields = N)], #[psio(positional)] and #[psio(returns(...))]";
    let mut cap = None;
    let mut returns = None;
    let mut max_fields: Option<u64> = None;
    let mut positional = false;
    for attr in attrs {
        if attr.path.is_ident("json") || attr.path.is_ident("serde") {
            return Err(syn::Error::new_spanned(attr, ACCEPTED));
        }
        if !attr.path.is_ident("pssz") && !attr.path.is_ident("psio") {
            continue;
        }
        let syn::Meta::List(list) = attr.parse_meta()? else {
            return Err(syn::Error::new_spanned(attr, ACCEPTED));
        };
        for item in &list.nested {
            match item {
                syn::NestedMeta::Meta(syn::Meta::NameValue(nv))
                    if attr.path.is_ident("pssz") && nv.path.is_ident("max_dynamic_data") =>
                {
                    let syn::Lit::Int(value) = &nv.lit else {
                        return Err(syn::Error::new_spanned(nv, "max_dynamic_data is an integer"));
                    };
                    if cap.replace(value.base10_parse()?).is_some() {
                        return Err(syn::Error::new_spanned(nv, "`max_dynamic_data` is given twice"));
                    }
                }
                syn::NestedMeta::Meta(syn::Meta::NameValue(nv))
                    if attr.path.is_ident("psio") && nv.path.is_ident("max_fields") =>
                {
                    let syn::Lit::Int(value) = &nv.lit else {
                        return Err(syn::Error::new_spanned(nv, "max_fields is an integer"));
                    };
                    let n: u64 = value.base10_parse()?;
                    if n > crate::attrs::MAX_RECORD_FIELDS as u64 {
                        return Err(syn::Error::new_spanned(nv, "max_fields is at most 255 (docs/pssz-spec.md 8.4)"));
                    }
                    if max_fields.replace(n).is_some() {
                        return Err(syn::Error::new_spanned(nv, "`max_fields` is given twice"));
                    }
                }
                syn::NestedMeta::Meta(syn::Meta::Path(path))
                    if attr.path.is_ident("psio") && path.is_ident("positional") =>
                {
                    positional = true;
                }
                syn::NestedMeta::Meta(syn::Meta::List(inner))
                    if attr.path.is_ident("psio") && inner.path.is_ident("returns") =>
                {
                    let bounds = &inner.nested;
                    if returns.replace(syn::parse_quote!(#[psio(#bounds)])).is_some() {
                        return Err(syn::Error::new_spanned(inner, "`returns` is given twice"));
                    }
                }
                other => return Err(syn::Error::new_spanned(other, ACCEPTED)),
            }
        }
    }
    Ok(MethodAnnotations { cap, returns, max_fields, positional })
}

/// The annotations of a parameter: `#[psio(...)]` and `#[json(...)]`, as
/// on a member of a record.
fn param_annotations(attrs: &[syn::Attribute]) -> syn::Result<Vec<syn::Attribute>> {
    let mut out = Vec::new();
    for attr in attrs {
        if attr.path.is_ident("psio") || attr.path.is_ident("json") {
            out.push(attr.clone());
        } else if attr.path.is_ident("pssz") || attr.path.is_ident("serde") {
            return Err(syn::Error::new_spanned(
                attr,
                "a parameter of a #[psio::interface] function takes #[psio(...)] and #[json(...)], as a \
                 member of a record does",
            ));
        }
    }
    Ok(out)
}

fn contains_reference(ty: &Type) -> bool {
    struct Find(bool);
    impl<'ast> syn::visit::Visit<'ast> for Find {
        fn visit_type_reference(&mut self, _: &'ast syn::TypeReference) {
            self.0 = true;
        }
        fn visit_lifetime(&mut self, _: &'ast syn::Lifetime) {
            self.0 = true;
        }
        fn visit_type_impl_trait(&mut self, _: &'ast syn::TypeImplTrait) {
            self.0 = true;
        }
    }
    let mut find = Find(false);
    syn::visit::Visit::visit_type(&mut find, ty);
    find.0
}

fn method(item: &syn::TraitItemMethod) -> syn::Result<Method> {
    let sig = &item.sig;
    let MethodAnnotations { cap, returns, max_fields, positional } = method_annotations(&item.attrs)?;
    if !sig.generics.params.is_empty() || sig.generics.where_clause.is_some() {
        return Err(syn::Error::new_spanned(&sig.generics, "a function of a #[psio::interface] is not generic"));
    }
    if let Some(token) = &sig.asyncness {
        return Err(syn::Error::new_spanned(token, "a function of a #[psio::interface] is not async"));
    }
    if let Some(token) = &sig.unsafety {
        return Err(syn::Error::new_spanned(token, "a function of a #[psio::interface] is not unsafe"));
    }
    if let Some(token) = &sig.variadic {
        return Err(syn::Error::new_spanned(token, "a function of a #[psio::interface] is not variadic"));
    }
    let mut receiver = Receiver::None;
    let mut params = Vec::new();
    for arg in &sig.inputs {
        match arg {
            FnArg::Receiver(this) => {
                if this.reference.is_none() {
                    return Err(syn::Error::new_spanned(
                        this,
                        "a method of a #[psio::interface] takes `&self` or `&mut self` (or no `self`)",
                    ));
                }
                receiver = if this.mutability.is_some() { Receiver::Mut } else { Receiver::Ref };
            }
            FnArg::Typed(arg) => {
                let attrs = param_annotations(&arg.attrs)?;
                let Pat::Ident(pat) = &*arg.pat else {
                    return Err(syn::Error::new_spanned(
                        &arg.pat,
                        "a parameter of a #[psio::interface] function has a name: `name: Type`",
                    ));
                };
                if pat.ident == "self" {
                    return Err(syn::Error::new_spanned(
                        arg,
                        "a method of a #[psio::interface] takes `&self` or `&mut self` (or no `self`)",
                    ));
                }
                if contains_reference(&arg.ty) {
                    return Err(syn::Error::new_spanned(
                        &arg.ty,
                        "a parameter of a #[psio::interface] function is an owned value (the arguments of a \
                         call are the tuple of the parameter types): write `String`, not `&str`",
                    ));
                }
                params.push(Param { ident: pat.ident.clone(), ty: (*arg.ty).clone(), attrs });
            }
        }
    }
    let result = match &sig.output {
        ReturnType::Default => None,
        ReturnType::Type(_, ty) => match &**ty {
            Type::Tuple(tuple) if tuple.elems.is_empty() => None,
            ty => {
                if contains_reference(ty) {
                    return Err(syn::Error::new_spanned(
                        ty,
                        "the result of a #[psio::interface] function is an owned value",
                    ));
                }
                Some(ty.clone())
            }
        },
    };
    if returns.is_some() && result.is_none() {
        return Err(syn::Error::new_spanned(&sig.ident, "returns(...) bounds a result, and this function has none"));
    }
    // docs/pssz-spec.md 8.4, 16: an argument list is a tuple.
    if params.len() > crate::attrs::MAX_RECORD_FIELDS {
        return Err(syn::Error::new_spanned(&sig.ident, "a function has at most 255 parameters"));
    }
    if let Some(max) = max_fields {
        if params.len() as u64 > max {
            return Err(syn::Error::new_spanned(
                &sig.ident,
                format!("max_fields = {max}, and the function has {} parameters", params.len()),
            ));
        }
    }
    Ok(Method {
        ident: sig.ident.clone(),
        name: unraw(&sig.ident),
        receiver,
        params,
        result,
        cap,
        returns,
        max_fields,
        positional,
    })
}

pub fn expand(options: Tokens, item: Tokens) -> syn::Result<Tokens> {
    let options: Options = syn::parse2(options)?;
    let mut item: ItemTrait = syn::parse2(item).map_err(|error| {
        syn::Error::new(error.span(), format!("#[psio::interface] is written on a trait: {error}"))
    })?;
    if !item.generics.params.is_empty() || item.generics.where_clause.is_some() {
        return Err(syn::Error::new_spanned(&item.generics, "a #[psio::interface] trait is not generic"));
    }
    let k: syn::Path = options.krate.clone().unwrap_or_else(|| syn::parse_quote!(::psio));
    let k_text = quote!(#k).to_string().replace(' ', "");
    let trait_ident = item.ident.clone();
    let interface_name = options.name.as_ref().map(LitStr::value).unwrap_or_else(|| unraw(&trait_ident));
    let module = options
        .module
        .clone()
        .unwrap_or_else(|| Ident::new(&snake_case(&unraw(&trait_ident)), trait_ident.span()));
    if module == trait_ident {
        return Err(syn::Error::new(
            trait_ident.span(),
            "the generated module would have the trait's own name; give `module = \"name\"`",
        ));
    }

    let mut methods = Vec::new();
    for member in &item.items {
        match member {
            TraitItem::Method(m) => methods.push(method(m)?),
            other => {
                return Err(syn::Error::new(
                    other.span(),
                    "a #[psio::interface] trait holds functions only",
                ))
            }
        }
    }
    for (i, m) in methods.iter().enumerate() {
        if methods[..i].iter().any(|other| other.name == m.name) {
            return Err(syn::Error::new(m.ident.span(), "two functions of an interface have one name"));
        }
        for (j, param) in m.params.iter().enumerate() {
            if m.params[..j].iter().any(|other| other.ident == param.ident) {
                return Err(syn::Error::new(param.ident.span(), "two parameters of a function have one name"));
            }
        }
    }
    // The annotations are consumed here: they are the argument record's.
    for member in &mut item.items {
        if let TraitItem::Method(m) = member {
            m.attrs.retain(|a| !["psio", "pssz"].iter().any(|n| a.path.is_ident(n)));
            for arg in &mut m.sig.inputs {
                if let FnArg::Typed(arg) = arg {
                    arg.attrs.retain(|a| !["psio", "pssz", "json"].iter().any(|n| a.path.is_ident(n)));
                }
            }
        }
    }

    let vis = &item.vis;
    // Inside the module, and one level deeper for the argument records, the
    // trait's own visibility is one step further away.
    let inner_vis = |levels: usize| -> Tokens {
        match vis {
            syn::Visibility::Inherited => {
                let supers = (0..levels).map(|_| quote!(super));
                quote!(pub(in #(#supers)::*))
            }
            syn::Visibility::Restricted(restricted) if restricted.in_token.is_none() => {
                let path = &restricted.path;
                if path.is_ident("self") {
                    let supers = (0..levels).map(|_| quote!(super));
                    quote!(pub(in #(#supers)::*))
                } else if path.is_ident("super") {
                    let supers = (0..=levels).map(|_| quote!(super));
                    quote!(pub(in #(#supers)::*))
                } else {
                    quote!(#vis)
                }
            }
            syn::Visibility::Restricted(restricted) => {
                let path = &restricted.path;
                if path.leading_colon.is_none()
                    && path.segments.first().is_some_and(|s| s.ident == "super" || s.ident == "self")
                {
                    let supers = (0..levels).map(|_| quote!(super));
                    let rest: Vec<_> = path.segments.iter().filter(|s| s.ident != "self").collect();
                    quote!(pub(in #(#supers::)* #(#rest)::*))
                } else {
                    quote!(#vis)
                }
            }
            other => quote!(#other),
        }
    };
    let vis1 = inner_vis(1);
    let vis2 = inner_vis(2);

    let derives = &options.derives;
    let method_count = methods.len();
    let mut records = Vec::new();
    let mut infos = Vec::new();
    let mut funcs = Vec::new();
    let mut argument_lists = Vec::new();
    let mut arms = Vec::new();
    let mut proxies = Vec::new();
    for (index, m) in methods.iter().enumerate() {
        let ident = &m.ident;
        let name = &m.name;
        let schema_name = format!("{interface_name}.{name}");
        let param_idents: Vec<_> = m.params.iter().map(|p| &p.ident).collect();
        let param_types: Vec<_> = m.params.iter().map(|p| &p.ty).collect();
        // The names the interface has for the parameters: none when the
        // method is positional.
        let param_names: Vec<_> =
            if m.positional { Vec::new() } else { m.params.iter().map(|p| unraw(&p.ident)).collect() };
        let func_param_names: Vec<_> =
            m.params
                .iter()
                .enumerate()
                // A positional parameter is named by its position ("0", "1",
                // …): the member names of its argument list, never empty.
                .map(|(i, p)| if m.positional { i.to_string() } else { unraw(&p.ident) })
                .collect();
        let named = !m.positional;
        let arguments = if named { "named" } else { "positional" };
        let record_max_fields = m.max_fields.map(|n| quote!(#[psio(max_fields = #n)]));
        let param_attrs: Vec<_> = m.params.iter().map(|p| &p.attrs).collect();
        // What each parameter is on the wire and in the schema.
        let param_wire: Vec<Type> =
            m.params.iter().map(|p| wire_type(&p.attrs, &p.ty, &k)).collect::<syn::Result<_>>()?;
        let record_cap = m.cap.map(|cap| quote!(#[pssz(max_dynamic_data = #cap)]));
        let returns: &[syn::Attribute] = m.returns.as_slice();
        let result_bound = match &m.result {
            Some(ty) => annotation(returns, ty, &k)?,
            None => None,
        };
        let has_result = m.result.is_some();
        let result_type = m.result.clone().unwrap_or_else(|| syn::parse_quote!(()));
        let (result_to, result_from) = match (&m.result, &result_bound) {
            // A bounded result is checked when it is written and read.
            (Some(_), Some(crate::attrs::Annotated { own, spec })) => (
                quote!(#k::pssz::try_to_pssz(&#k::pssz::bounds::Described::<#own, #spec>::new(result))),
                quote! {
                    #k::pssz::from_pssz::<<#own as #k::pssz::bounds::Bounds<#spec>>::As>(bytes)
                        .map(<#own as #k::pssz::bounds::Bounds<#spec>>::from_as)
                },
            ),
            (Some(_), None) => (
                quote!(#k::pssz::try_to_pssz(result)),
                quote!(#k::pssz::from_pssz::<Self::Result>(bytes)),
            ),
            (None, _) => (
                quote!(::core::result::Result::Ok(::std::vec::Vec::new())),
                quote! {
                    if bytes.is_empty() {
                        ::core::result::Result::Ok(())
                    } else {
                        ::core::result::Result::Err(#k::pssz::PsszError::new(
                            "interface: a method with no result returned bytes",
                        ))
                    }
                },
            ),
        };
        // The result as `returns(...)` types it: its type, its validator,
        // the trusted reader and its Schema IR node.
        let (result_as, result_validate, result_trusted) = match (&m.result, &result_bound) {
            (Some(_), Some(crate::attrs::Annotated { own, spec })) => (
                quote!(<#own as #k::pssz::bounds::Bounds<#spec>>::As),
                quote!(#k::pssz::validate_pssz::<<#own as #k::pssz::bounds::Bounds<#spec>>::As>(bytes)),
                quote! {
                    <#own as #k::pssz::bounds::Bounds<#spec>>::from_as(
                        #k::pssz::from_pssz_trusted::<<#own as #k::pssz::bounds::Bounds<#spec>>::As>(bytes),
                    )
                },
            ),
            (Some(ty), None) => (
                quote!(#ty),
                quote!(#k::pssz::validate_pssz::<Self::Result>(bytes)),
                quote!(#k::pssz::from_pssz_trusted::<Self::Result>(bytes)),
            ),
            (None, _) => (
                quote!(()),
                quote! {
                    if bytes.is_empty() {
                        ::core::result::Result::Ok(())
                    } else {
                        ::core::result::Result::Err(#k::pssz::PsszError::new(
                            "interface: a method with no result returned bytes",
                        ))
                    }
                },
                quote!(()),
            ),
        };
        let result_node = match (&m.result, &result_bound) {
            (Some(ty), Some(crate::attrs::Annotated { own, spec })) => {
                let wire = wire_type(returns, ty, &k)?;
                quote! {
                    let _ = <<#own as #k::pssz::bounds::SchemaAs<#spec>>::Plain
                        as #k::pssz::schema_ir::ToPsszSchema>::schema(builder)?;
                    let __node = <#own as #k::pssz::bounds::SchemaAs<#spec>>::annotated(builder)?;
                    #k::interface::bare_result::<#wire>(Self::NAME, __node)
                }
            }
            (Some(ty), None) => {
                let wire = wire_type(returns, ty, &k)?;
                quote! {
                    let __node = <#ty as #k::pssz::schema_ir::ToPsszSchema>::schema(builder)?;
                    #k::interface::bare_result::<#wire>(Self::NAME, __node)
                }
            }
            (None, _) => quote!(::core::result::Result::Ok(::core::option::Option::None)),
        };
        let doc = format!("The arguments of `{}::{}`.", unraw(&trait_ident), name);
        records.push(quote! {
            #[doc = #doc]
            #[allow(non_camel_case_types)]
            #[derive(#k::PsszPack, #k::PsszUnpack, #k::ToPsszSchema, #k::JsonText #(, #derives)*)]
            #[psio(crate = #k_text, name = #schema_name, arguments = #arguments)]
            #record_max_fields
            #record_cap
            #vis2 struct #ident { #(#(#param_attrs)* pub #param_idents: #param_types),* }

            impl #ident {
                /// The index of the method in the interface.
                pub const INDEX: usize = #index;
            }
            impl ::core::convert::From<(#(#param_types,)*)> for #ident {
                #[allow(unused_variables)]
                fn from(tuple: (#(#param_types,)*)) -> Self {
                    let (#(#param_idents,)*) = tuple;
                    Self { #(#param_idents),* }
                }
            }
            impl ::core::convert::From<#ident> for (#(#param_types,)*) {
                #[allow(unused_variables)]
                fn from(args: #ident) -> Self {
                    (#(args.#param_idents,)*)
                }
            }
            impl #k::interface::Method for #ident {
                type Interface = super::Tag;
                type Tuple = (#(#param_wire,)*);
                type Result = #result_type;
                type ResultAs = #result_as;
                const INDEX: usize = #index;
                const NAME: &'static str = #name;
                const PARAM_NAMES: &'static [&'static str] = &[#(#param_names),*];
                const NAMED: bool = #named;
                const HAS_RESULT: bool = #has_result;
                #[allow(unused_variables)]
                fn result_to_pssz(result: &Self::Result) -> #k::pssz::PsszResult<::std::vec::Vec<u8>> {
                    #result_to
                }
                fn result_from_pssz(bytes: &[u8]) -> #k::pssz::PsszResult<Self::Result> {
                    #result_from
                }
                #[allow(unused_variables)]
                fn result_from_pssz_trusted(bytes: &[u8]) -> Self::Result {
                    #result_trusted
                }
                fn validate_result(bytes: &[u8]) -> #k::pssz::PsszResult<()> {
                    #result_validate
                }
                #[allow(unused_variables)]
                fn result_schema(
                    builder: &mut #k::pssz::schema_ir::SchemaBuilder,
                ) -> #k::pssz::PsszResult<::core::option::Option<#k::pssz::schema_ir::AnyType>> {
                    #result_node
                }
            }
        });
        let receiver = match m.receiver {
            Receiver::None => quote!(None),
            Receiver::Ref => quote!(Ref),
            Receiver::Mut => quote!(Mut),
        };
        infos.push(quote! {
            #k::interface::MethodInfo {
                name: #name,
                params: &[#(#param_names),*],
                named: #named,
                receiver: #k::interface::Receiver::#receiver,
                has_result: #has_result,
            }
        });
        // C++ `build_func`: the plain type of each parameter and of the
        // result is interned first, in order ...
        let mut plain = Vec::new();
        // ... then what the declaration says of them
        // (`apply_member_annotations`): the annotated type, inline.
        let mut annotated_params = Vec::new();
        for (i, p) in m.params.iter().enumerate() {
            let slot = quote::format_ident!("__plain_{}", i);
            let ty = &p.ty;
            match annotation(&p.attrs, ty, &k)? {
                Some(crate::attrs::Annotated { own, spec }) => {
                    plain.push(quote! {
                        let #slot = <<#own as #k::pssz::bounds::SchemaAs<#spec>>::Plain
                            as #k::pssz::schema_ir::ToPsszSchema>::schema(builder)?;
                    });
                    annotated_params.push(quote!(<#own as #k::pssz::bounds::SchemaAs<#spec>>::annotated(builder)?));
                }
                None => {
                    plain.push(quote! {
                        let #slot = <#ty as #k::pssz::schema_ir::ToPsszSchema>::schema(builder)?;
                    });
                    annotated_params.push(quote!(#slot));
                }
            }
        }
        let (result_schema, result_attributes) = match &m.result {
            Some(ty) => {
                let wire = wire_type(returns, ty, &k)?;
                let node = match &result_bound {
                    Some(crate::attrs::Annotated { own, spec }) => {
                        plain.push(quote! {
                            let __plain_result = <<#own as #k::pssz::bounds::SchemaAs<#spec>>::Plain
                                as #k::pssz::schema_ir::ToPsszSchema>::schema(builder)?;
                        });
                        quote!(<#own as #k::pssz::bounds::SchemaAs<#spec>>::annotated(builder)?)
                    }
                    None => {
                        plain.push(quote! {
                            let __plain_result = <#ty as #k::pssz::schema_ir::ToPsszSchema>::schema(builder)?;
                        });
                        quote!(__plain_result)
                    }
                };
                (
                    quote!(::core::option::Option::Some(::std::boxed::Box::new(#node))),
                    quote!(#k::interface::result_attributes::<#wire>()),
                )
            }
            None => (quote!(::core::option::Option::None), quote!(::std::vec::Vec::new())),
        };
        // The parameter-count cap, then the cap of the argument record
        // (its size limit and its width): the order C++ writes.
        let max_fields_attribute = match m.max_fields {
            Some(max) => {
                let max = max.to_string();
                quote! {
                    __attributes.push(#k::pssz::schema_ir::Attribute {
                        name: "maxFields".into(),
                        value: ::core::option::Option::Some(#max.into()),
                    });
                }
            }
            None => quote!(),
        };
        let cap_attribute = match m.cap {
            Some(cap) => {
                let cap = cap.to_string();
                quote! {
                    __attributes.push(#k::pssz::schema_ir::Attribute {
                        name: "maxDynamicData".into(),
                        value: ::core::option::Option::Some(#cap.into()),
                    });
                }
            }
            None => quote!(),
        };
        funcs.push(quote! {{
            #(#plain)*
            #k::pssz::schema_ir::Func {
                name: #name.into(),
                params: ::std::vec![#(#k::interface::param::<#param_wire>(#func_param_names, #annotated_params)),*],
                result: #result_schema,
                attributes: {
                    #[allow(unused_mut)]
                    let mut __attributes: ::std::vec::Vec<#k::pssz::schema_ir::Attribute> = #result_attributes;
                    #max_fields_attribute
                    #cap_attribute
                    __attributes
                },
            }
        }});
        argument_lists.push(quote!(builder.insert_named::<args::#ident>(#schema_name)?;));
        let call = match m.receiver {
            Receiver::None => quote!(<__T as #trait_ident>::#ident(#(__args.#param_idents),*)),
            _ => quote!(<__T as #trait_ident>::#ident(target #(, __args.#param_idents)*)),
        };
        arms.push(quote! {
            #index => {
                #[allow(unused_variables)]
                let __args = #k::pssz::from_pssz::<args::#ident>(arguments)
                    .map_err(#k::interface::DispatchError::Arguments)?;
                let __result = #call;
                <args::#ident as #k::interface::Method>::result_to_pssz(&__result)
                    .map_err(#k::interface::DispatchError::Result)
            }
        });
        let doc = format!(
            "Hands the arguments of `{}::{}` to the caller.",
            unraw(&trait_ident),
            name
        );
        proxies.push(quote! {
            #[doc = #doc]
            #[allow(clippy::too_many_arguments)]
            pub fn #ident(&self #(, #param_idents: #param_types)*) -> __C::Output<#result_type> {
                self.0.call(args::#ident { #(#param_idents),* })
            }
        });
    }

    let package = match (&options.package, &options.version) {
        (Some(package), version) => {
            let version = version.as_ref().map(LitStr::value).unwrap_or_default();
            quote!(::core::option::Option::Some((#package, #version)))
        }
        _ => quote!(::core::option::Option::None),
    };
    let owned_types = &options.types;
    let trait_text = unraw(&trait_ident);
    let module_doc = format!(
        "The interface of the trait `{trait_text}` (`#[psio::interface]`): the tag, the argument \
         records, dispatch and the proxy."
    );
    let tag_doc = format!("The interface `{interface_name}` as a type.");
    let dispatch_doc = format!(
        "Calls method `method` of `{trait_text}` on `target` with the encoded `arguments` (validated) \
         and returns the encoded result (no bytes for a method that returns nothing)."
    );

    Ok(quote! {
        #item

        #[doc = #module_doc]
        #vis mod #module {
            #[allow(unused_imports)]
            use super::*;

            #[doc = #tag_doc]
            #[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
            #vis1 struct Tag;

            impl #k::interface::Interface for Tag {
                const NAME: &'static str = #interface_name;
                const PACKAGE: ::core::option::Option<(&'static str, &'static str)> = #package;
                const METHODS: &'static [#k::interface::MethodInfo] = &[#(#infos),*];
                #[allow(unused_variables)]
                fn build_interface(
                    builder: &mut #k::pssz::schema_ir::SchemaBuilder,
                ) -> #k::pssz::PsszResult<#k::pssz::schema_ir::Interface> {
                    // The order C++ SchemaBuilder::build_interface walks:
                    // the owned types, then per function the plain types of
                    // its parameters and result, then their annotated types.
                    let type_names = ::std::vec![
                        #(#k::interface::type_name_for::<#owned_types>(builder)?),*
                    ];
                    let funcs = ::std::vec![#(#funcs),*];
                    ::core::result::Result::Ok(#k::pssz::schema_ir::Interface {
                        name: #interface_name.into(),
                        type_names,
                        funcs,
                        attributes: ::std::vec![],
                    })
                }
                #[allow(unused_variables)]
                fn insert_argument_lists(
                    builder: &mut #k::pssz::schema_ir::SchemaBuilder,
                ) -> #k::pssz::PsszResult<()> {
                    #(#argument_lists)*
                    ::core::result::Result::Ok(())
                }
            }

            /// The argument record of each method, named after it. Its pSSZ
            /// is that of the tuple of the method's parameters.
            #vis1 mod args {
                #[allow(unused_imports)]
                use super::super::*;
                #(#records)*
            }

            /// The number of methods.
            #vis1 const METHOD_COUNT: usize = #method_count;

            #[doc = #dispatch_doc]
            #[allow(unused_variables)]
            #vis1 fn dispatch<__T: #trait_ident>(
                target: &mut __T,
                method: usize,
                arguments: &[u8],
            ) -> ::core::result::Result<::std::vec::Vec<u8>, #k::interface::DispatchError> {
                match method {
                    #(#arms)*
                    _ => ::core::result::Result::Err(#k::interface::DispatchError::UnknownMethod),
                }
            }

            /// [`dispatch`] by the method's name.
            #vis1 fn dispatch_name<__T: #trait_ident>(
                target: &mut __T,
                method: &str,
                arguments: &[u8],
            ) -> ::core::result::Result<::std::vec::Vec<u8>, #k::interface::DispatchError> {
                match <Tag as #k::interface::Interface>::index_of_method(method) {
                    ::core::option::Option::Some(index) => dispatch(target, index, arguments),
                    ::core::option::Option::None => {
                        ::core::result::Result::Err(#k::interface::DispatchError::UnknownMethod)
                    }
                }
            }

            /// One function per method; each hands its argument record to
            /// the caller.
            #[derive(Debug, Clone, Copy, Default)]
            #vis1 struct Proxy<__C>(pub __C);

            impl<__C: #k::interface::Caller> Proxy<__C> {
                #(#proxies)*
            }
        }
    })
}
