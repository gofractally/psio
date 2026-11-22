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

struct Method {
    ident: Ident,
    /// The identifier as text, without `r#`.
    name: String,
    receiver: Receiver,
    params: Vec<(Ident, Type)>,
    /// `None`: the function returns nothing.
    result: Option<Type>,
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

/// An annotation attribute on a method or a parameter: not supported until
/// member bounds exist in Rust; their place is the argument record.
fn refuse_annotations(attrs: &[syn::Attribute], what: &str) -> syn::Result<()> {
    for attr in attrs {
        for namespace in ["psio", "pssz", "json", "serde"] {
            if attr.path.is_ident(namespace) {
                return Err(syn::Error::new_spanned(
                    attr,
                    format!(
                        "#[{namespace}(...)] on {what} of a #[psio::interface] is not supported yet: \
                         parameter annotations and argument caps arrive with member bounds"
                    ),
                ));
            }
        }
    }
    Ok(())
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
    refuse_annotations(&item.attrs, "a method")?;
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
                refuse_annotations(&arg.attrs, "a parameter")?;
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
                params.push((pat.ident.clone(), (*arg.ty).clone()));
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
    Ok(Method { ident: sig.ident.clone(), name: unraw(&sig.ident), receiver, params, result })
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
        for (j, (param, _)) in m.params.iter().enumerate() {
            if m.params[..j].iter().any(|(other, _)| other == param) {
                return Err(syn::Error::new(param.span(), "two parameters of a function have one name"));
            }
        }
    }
    // Parameter attributes are consumed here (none is accepted yet).
    for member in &mut item.items {
        if let TraitItem::Method(m) = member {
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
    let mut arms = Vec::new();
    let mut proxies = Vec::new();
    for (index, m) in methods.iter().enumerate() {
        let ident = &m.ident;
        let name = &m.name;
        let schema_name = format!("{interface_name}.{name}");
        let param_idents: Vec<_> = m.params.iter().map(|(ident, _)| ident).collect();
        let param_types: Vec<_> = m.params.iter().map(|(_, ty)| ty).collect();
        let param_names: Vec<_> = m.params.iter().map(|(ident, _)| unraw(ident)).collect();
        let has_result = m.result.is_some();
        let result_type = m.result.clone().unwrap_or_else(|| syn::parse_quote!(()));
        let (result_to, result_from) = match &m.result {
            Some(_) => (
                quote!(#k::pssz::try_to_pssz(result)),
                quote!(#k::pssz::from_pssz::<Self::Result>(bytes)),
            ),
            None => (
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
        let doc = format!("The arguments of `{}::{}`.", unraw(&trait_ident), name);
        records.push(quote! {
            #[doc = #doc]
            #[allow(non_camel_case_types)]
            #[derive(#k::PsszPack, #k::PsszUnpack, #k::ToPsszSchema, #k::JsonText #(, #derives)*)]
            #[psio(crate = #k_text, name = #schema_name)]
            #vis2 struct #ident { #(pub #param_idents: #param_types),* }

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
                type Tuple = (#(#param_types,)*);
                type Result = #result_type;
                const INDEX: usize = #index;
                const NAME: &'static str = #name;
                const PARAM_NAMES: &'static [&'static str] = &[#(#param_names),*];
                const HAS_RESULT: bool = #has_result;
                #[allow(unused_variables)]
                fn result_to_pssz(result: &Self::Result) -> #k::pssz::PsszResult<::std::vec::Vec<u8>> {
                    #result_to
                }
                fn result_from_pssz(bytes: &[u8]) -> #k::pssz::PsszResult<Self::Result> {
                    #result_from
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
                receiver: #k::interface::Receiver::#receiver,
                has_result: #has_result,
            }
        });
        let result_schema = match &m.result {
            Some(ty) => quote! {
                ::core::option::Option::Some(::std::boxed::Box::new(
                    <#ty as #k::pssz::schema_ir::ToPsszSchema>::schema(builder)?,
                ))
            },
            None => quote!(::core::option::Option::None),
        };
        funcs.push(quote! {
            #k::pssz::schema_ir::Func {
                name: #name.into(),
                params: ::std::vec![#(#k::interface::param::<#param_types>(builder, #param_names)?),*],
                result: #result_schema,
                attributes: ::std::vec![],
            }
        });
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
                    // the owned types, then each function's parameters and
                    // result.
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
