use crate::fracpack_macro::Options;
use darling::FromDeriveInput;
use proc_macro::TokenStream;
use proc_macro2::TokenStream as TokenStream2;
use quote::quote;
use std::str::FromStr;
use syn::{parse_macro_input, Data, DataEnum, DataStruct, DeriveInput, Fields};

pub fn to_canonical_json_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);

    let opts = match Options::from_derive_input(&input) {
        Ok(val) => val,
        Err(err) => return err.write_errors().into(),
    };
    let fracpack_mod = TokenStream2::from_str(&opts.fracpack_mod).unwrap();

    match &input.data {
        Data::Struct(data) => to_json_struct(&fracpack_mod, &input, data),
        Data::Enum(data) => to_json_enum(&fracpack_mod, &input, data),
        Data::Union(_) => panic!("ToCanonicalJson does not support unions"),
    }
}

pub fn from_canonical_json_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);

    let opts = match Options::from_derive_input(&input) {
        Ok(val) => val,
        Err(err) => return err.write_errors().into(),
    };
    let fracpack_mod = TokenStream2::from_str(&opts.fracpack_mod).unwrap();

    match &input.data {
        Data::Struct(data) => from_json_struct(&fracpack_mod, &input, data),
        Data::Enum(data) => from_json_enum(&fracpack_mod, &input, data),
        Data::Union(_) => panic!("FromCanonicalJson does not support unions"),
    }
}

// ============================================================
// ToCanonicalJson for structs
// ============================================================

fn to_json_struct(
    fracpack_mod: &TokenStream2,
    input: &DeriveInput,
    data: &DataStruct,
) -> TokenStream {
    let name = &input.ident;
    let (impl_generics, ty_generics, where_clause) = input.generics.split_for_impl();

    match &data.fields {
        Fields::Named(named) => {
            let field_inserts = named.named.iter().map(|field| {
                let field_name = field.ident.as_ref().unwrap();
                let field_name_str = field_name.to_string();
                let ty = &field.ty;
                if is_vec_u8_type(ty) {
                    // Vec<u8> -> hex string
                    quote! {
                        builder.field_value(
                            #field_name_str,
                            serde_json::Value::String(#fracpack_mod::json::bytes_to_hex(&self.#field_name)),
                        );
                    }
                } else if is_option_vec_u8_type(ty) {
                    // Option<Vec<u8>> -> hex string or null
                    quote! {
                        builder.field_value(
                            #field_name_str,
                            match &self.#field_name {
                                None => serde_json::Value::Null,
                                Some(v) => serde_json::Value::String(#fracpack_mod::json::bytes_to_hex(v)),
                            },
                        );
                    }
                } else {
                    // All other types: use ToCanonicalJson trait
                    quote! {
                        builder.field(#field_name_str, &self.#field_name);
                    }
                }
            });

            TokenStream::from(quote! {
                impl #impl_generics #fracpack_mod::json::ToCanonicalJson for #name #ty_generics #where_clause {
                    fn to_json_value(&self) -> serde_json::Value {
                        let mut builder = #fracpack_mod::json::JsonObjectBuilder::new();
                        #(#field_inserts)*
                        builder.build()
                    }
                }
            })
        }
        Fields::Unnamed(unnamed) => {
            if unnamed.unnamed.len() == 1 {
                // Newtype struct: delegate to inner
                TokenStream::from(quote! {
                    impl #impl_generics #fracpack_mod::json::ToCanonicalJson for #name #ty_generics #where_clause {
                        fn to_json_value(&self) -> serde_json::Value {
                            self.0.to_json_value()
                        }
                    }
                })
            } else {
                // Tuple struct: serialize as array
                let indices = (0..unnamed.unnamed.len()).map(syn::Index::from);
                TokenStream::from(quote! {
                    impl #impl_generics #fracpack_mod::json::ToCanonicalJson for #name #ty_generics #where_clause {
                        fn to_json_value(&self) -> serde_json::Value {
                            serde_json::Value::Array(vec![
                                #(self.#indices.to_json_value()),*
                            ])
                        }
                    }
                })
            }
        }
        Fields::Unit => {
            TokenStream::from(quote! {
                impl #impl_generics #fracpack_mod::json::ToCanonicalJson for #name #ty_generics #where_clause {
                    fn to_json_value(&self) -> serde_json::Value {
                        serde_json::Value::Null
                    }
                }
            })
        }
    }
}

// ============================================================
// FromCanonicalJson for structs
// ============================================================

fn from_json_struct(
    fracpack_mod: &TokenStream2,
    input: &DeriveInput,
    data: &DataStruct,
) -> TokenStream {
    let name = &input.ident;
    let (impl_generics, ty_generics, where_clause) = input.generics.split_for_impl();

    match &data.fields {
        Fields::Named(named) => {
            let field_reads = named.named.iter().map(|field| {
                let field_name = field.ident.as_ref().unwrap();
                let field_name_str = field_name.to_string();
                let ty = &field.ty;
                if is_vec_u8_type(ty) {
                    // hex string -> Vec<u8>
                    quote! {
                        #field_name: #fracpack_mod::json::bytes_from_json_value(reader.field_value(#field_name_str))?,
                    }
                } else if is_option_vec_u8_type(ty) {
                    // hex string or null -> Option<Vec<u8>>
                    quote! {
                        #field_name: match reader.field_value(#field_name_str) {
                            serde_json::Value::Null => None,
                            v => Some(#fracpack_mod::json::bytes_from_json_value(v)?),
                        },
                    }
                } else {
                    quote! {
                        #field_name: reader.field::<#ty>(#field_name_str)?,
                    }
                }
            });

            TokenStream::from(quote! {
                impl #impl_generics #fracpack_mod::json::FromCanonicalJson for #name #ty_generics #where_clause {
                    fn from_json_value(v: &serde_json::Value) -> Result<Self, #fracpack_mod::json::JsonError> {
                        let reader = #fracpack_mod::json::JsonObjectReader::new(v)?;
                        Ok(#name {
                            #(#field_reads)*
                        })
                    }
                }
            })
        }
        Fields::Unnamed(unnamed) => {
            if unnamed.unnamed.len() == 1 {
                let inner = &unnamed.unnamed[0].ty;
                TokenStream::from(quote! {
                    impl #impl_generics #fracpack_mod::json::FromCanonicalJson for #name #ty_generics #where_clause {
                        fn from_json_value(v: &serde_json::Value) -> Result<Self, #fracpack_mod::json::JsonError> {
                            Ok(#name(<#inner as #fracpack_mod::json::FromCanonicalJson>::from_json_value(v)?))
                        }
                    }
                })
            } else {
                let count = unnamed.unnamed.len();
                let indices = (0..count).map(syn::Index::from);
                let types = unnamed.unnamed.iter().map(|f| &f.ty);
                TokenStream::from(quote! {
                    impl #impl_generics #fracpack_mod::json::FromCanonicalJson for #name #ty_generics #where_clause {
                        fn from_json_value(v: &serde_json::Value) -> Result<Self, #fracpack_mod::json::JsonError> {
                            match v {
                                serde_json::Value::Array(arr) => {
                                    Ok(#name(
                                        #(<#types as #fracpack_mod::json::FromCanonicalJson>::from_json_value(
                                            arr.get(#indices).unwrap_or(&serde_json::Value::Null)
                                        )?),*
                                    ))
                                }
                                _ => Err(#fracpack_mod::json::JsonError::TypeMismatch {
                                    expected: "array",
                                    got: format!("{:?}", v),
                                }),
                            }
                        }
                    }
                })
            }
        }
        Fields::Unit => {
            TokenStream::from(quote! {
                impl #impl_generics #fracpack_mod::json::FromCanonicalJson for #name #ty_generics #where_clause {
                    fn from_json_value(_v: &serde_json::Value) -> Result<Self, #fracpack_mod::json::JsonError> {
                        Ok(#name)
                    }
                }
            })
        }
    }
}

// ============================================================
// ToCanonicalJson for enums (variant format)
// ============================================================

fn get_variant_json_name(variant: &syn::Variant) -> String {
    // Check for #[fracpack(name = "...")] attribute
    for attr in &variant.attrs {
        if attr.path.is_ident("fracpack") {
            if let Ok(syn::Meta::List(meta_list)) = attr.parse_meta() {
                for nested in &meta_list.nested {
                    if let syn::NestedMeta::Meta(syn::Meta::NameValue(nv)) = nested {
                        if nv.path.is_ident("name") {
                            if let syn::Lit::Str(lit) = &nv.lit {
                                return lit.value();
                            }
                        }
                    }
                }
            }
        }
    }
    // Default: use Rust variant name
    variant.ident.to_string()
}

fn to_json_enum(
    fracpack_mod: &TokenStream2,
    input: &DeriveInput,
    data: &DataEnum,
) -> TokenStream {
    let name = &input.ident;
    let (impl_generics, ty_generics, where_clause) = input.generics.split_for_impl();

    let arms = data.variants.iter().map(|variant| {
        let variant_ident = &variant.ident;
        let json_name = get_variant_json_name(variant);

        match &variant.fields {
            Fields::Unnamed(fields) if fields.unnamed.len() == 1 => {
                quote! {
                    #name::#variant_ident(inner) => {
                        #fracpack_mod::json::variant_to_json(#json_name, inner)
                    }
                }
            }
            Fields::Unnamed(fields) => {
                let field_bindings: Vec<_> = (0..fields.unnamed.len())
                    .map(|i| syn::Ident::new(&format!("f{}", i), proc_macro2::Span::call_site()))
                    .collect();
                let tuple_build = if fields.unnamed.is_empty() {
                    quote! { serde_json::Value::Null }
                } else {
                    quote! {
                        serde_json::Value::Array(vec![#(#field_bindings.to_json_value()),*])
                    }
                };
                quote! {
                    #name::#variant_ident(#(#field_bindings),*) => {
                        #fracpack_mod::json::variant_to_json_value(#json_name, #tuple_build)
                    }
                }
            }
            Fields::Named(fields) => {
                let field_names: Vec<_> = fields
                    .named
                    .iter()
                    .map(|f| f.ident.as_ref().unwrap())
                    .collect();
                let field_name_strs: Vec<_> = field_names.iter().map(|n| n.to_string()).collect();
                quote! {
                    #name::#variant_ident { #(#field_names),* } => {
                        let mut builder = #fracpack_mod::json::JsonObjectBuilder::new();
                        #(builder.field(#field_name_strs, #field_names);)*
                        #fracpack_mod::json::variant_to_json_value(#json_name, builder.build())
                    }
                }
            }
            Fields::Unit => {
                quote! {
                    #name::#variant_ident => {
                        #fracpack_mod::json::variant_to_json_value(#json_name, serde_json::Value::Null)
                    }
                }
            }
        }
    });

    TokenStream::from(quote! {
        impl #impl_generics #fracpack_mod::json::ToCanonicalJson for #name #ty_generics #where_clause {
            fn to_json_value(&self) -> serde_json::Value {
                use #fracpack_mod::json::ToCanonicalJson;
                match self {
                    #(#arms)*
                }
            }
        }
    })
}

// ============================================================
// FromCanonicalJson for enums
// ============================================================

fn from_json_enum(
    fracpack_mod: &TokenStream2,
    input: &DeriveInput,
    data: &DataEnum,
) -> TokenStream {
    let name = &input.ident;
    let (impl_generics, ty_generics, where_clause) = input.generics.split_for_impl();

    let arms = data.variants.iter().map(|variant| {
        let variant_ident = &variant.ident;
        let json_name = get_variant_json_name(variant);

        match &variant.fields {
            Fields::Unnamed(fields) if fields.unnamed.len() == 1 => {
                let inner_ty = &fields.unnamed[0].ty;
                quote! {
                    #json_name => {
                        Ok(#name::#variant_ident(
                            <#inner_ty as #fracpack_mod::json::FromCanonicalJson>::from_json_value(inner_val)?
                        ))
                    }
                }
            }
            Fields::Unnamed(fields) => {
                let count = fields.unnamed.len();
                if count == 0 {
                    quote! {
                        #json_name => Ok(#name::#variant_ident()),
                    }
                } else {
                    let indices = (0..count).map(syn::Index::from);
                    let types: Vec<_> = fields.unnamed.iter().map(|f| &f.ty).collect();
                    quote! {
                        #json_name => {
                            match inner_val {
                                serde_json::Value::Array(arr) => {
                                    Ok(#name::#variant_ident(
                                        #(<#types as #fracpack_mod::json::FromCanonicalJson>::from_json_value(
                                            arr.get(#indices).unwrap_or(&serde_json::Value::Null)
                                        )?),*
                                    ))
                                }
                                _ => Err(#fracpack_mod::json::JsonError::TypeMismatch {
                                    expected: "array",
                                    got: format!("{:?}", inner_val),
                                }),
                            }
                        }
                    }
                }
            }
            Fields::Named(fields) => {
                let field_names: Vec<_> = fields
                    .named
                    .iter()
                    .map(|f| f.ident.as_ref().unwrap())
                    .collect();
                let field_name_strs: Vec<_> = field_names.iter().map(|n| n.to_string()).collect();
                let field_types: Vec<_> = fields.named.iter().map(|f| &f.ty).collect();
                quote! {
                    #json_name => {
                        let reader = #fracpack_mod::json::JsonObjectReader::new(inner_val)?;
                        Ok(#name::#variant_ident {
                            #(#field_names: reader.field::<#field_types>(#field_name_strs)?),*
                        })
                    }
                }
            }
            Fields::Unit => {
                quote! {
                    #json_name => Ok(#name::#variant_ident),
                }
            }
        }
    });

    TokenStream::from(quote! {
        impl #impl_generics #fracpack_mod::json::FromCanonicalJson for #name #ty_generics #where_clause {
            fn from_json_value(v: &serde_json::Value) -> Result<Self, #fracpack_mod::json::JsonError> {
                let (case_name, inner_val) = #fracpack_mod::json::variant_from_json(v)?;
                match case_name {
                    #(#arms)*
                    other => Err(#fracpack_mod::json::JsonError::UnknownVariant(other.to_string())),
                }
            }
        }
    })
}

// ============================================================
// Type inspection helpers
// ============================================================

fn is_vec_u8_type(ty: &syn::Type) -> bool {
    if let syn::Type::Path(path) = ty {
        if let Some(seg) = path.path.segments.last() {
            if seg.ident == "Vec" {
                if let syn::PathArguments::AngleBracketed(args) = &seg.arguments {
                    if args.args.len() == 1 {
                        if let syn::GenericArgument::Type(syn::Type::Path(inner_path)) =
                            &args.args[0]
                        {
                            if let Some(inner_seg) = inner_path.path.segments.last() {
                                return inner_seg.ident == "u8";
                            }
                        }
                    }
                }
            }
        }
    }
    false
}

fn is_option_vec_u8_type(ty: &syn::Type) -> bool {
    // Option<Vec<u8>>
    if let syn::Type::Path(path) = ty {
        if let Some(seg) = path.path.segments.last() {
            if seg.ident == "Option" {
                if let syn::PathArguments::AngleBracketed(args) = &seg.arguments {
                    if args.args.len() == 1 {
                        if let syn::GenericArgument::Type(inner) = &args.args[0] {
                            return is_vec_u8_type(inner);
                        }
                    }
                }
            }
        }
    }
    false
}
