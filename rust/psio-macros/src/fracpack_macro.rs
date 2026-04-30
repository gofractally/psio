use darling::{FromDeriveInput, FromField, FromVariant};
use proc_macro::TokenStream;
use quote::quote;
use std::str::FromStr;
use syn::{
    parse_macro_input, Data, DataEnum, DataStruct, DeriveInput, Field, Fields, FieldsNamed,
    FieldsUnnamed, LitStr,
};

/// Fracpack struct level options
#[derive(Debug, FromDeriveInput, FromVariant)]
#[darling(default, attributes(fracpack))]
pub(crate) struct Options {
    pub(crate) definition_will_not_change: bool,
    pub(crate) fracpack_mod: String,
    pub(crate) custom: Option<LitStr>,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            definition_will_not_change: false,
            fracpack_mod: "psio".into(),
            custom: None,
        }
    }
}

/// Fracpack field level options
#[derive(Debug, Default, FromField)]
#[darling(default, attributes(fracpack))]
pub(crate) struct FieldOptions {
    pub(crate) skip: bool,
}

struct StructField<'a> {
    name: &'a proc_macro2::Ident,
    ty: &'a syn::Type,
    skip: bool,
}

struct EnumField<'a> {
    name: &'a proc_macro2::Ident,
    as_type: proc_macro2::TokenStream,
    selector: proc_macro2::TokenStream,
    pack: proc_macro2::TokenStream,
    unpack: proc_macro2::TokenStream,
}

pub(crate) fn skip_field(field: &Field) -> bool {
    FieldOptions::from_field(field).map_or(false, |attr| attr.skip)
}

fn use_field(field: &&StructField) -> bool {
    !field.skip
}

fn struct_fields(data: &DataStruct) -> Vec<StructField<'_>> {
    match &data.fields {
        Fields::Named(named) => named
            .named
            .iter()
            .map(|field| StructField {
                name: field.ident.as_ref().unwrap(),
                ty: &field.ty,
                skip: skip_field(field),
            })
            .collect(),
        Fields::Unnamed(_) => unimplemented!(),
        Fields::Unit => unimplemented!("fracpack does not support unit struct"),
    }
}

fn enum_named<'a>(
    fracpack_mod: &proc_macro2::TokenStream,
    enum_name: &proc_macro2::Ident,
    field_name: &'a proc_macro2::Ident,
    field: &FieldsNamed,
) -> EnumField<'a> {
    let as_type = {
        let types = field
            .named
            .iter()
            .map(|x| {
                let ty = &x.ty;
                quote! {#ty}
            })
            .reduce(|acc, new| quote! {#acc,#new})
            .unwrap_or_default();
        quote! {(#types,)}
    };
    let as_tuple_of_ref = {
        let types = field
            .named
            .iter()
            .map(|x| {
                let ty = &x.ty;
                quote! {&#ty}
            })
            .reduce(|acc, new| quote! {#acc,#new})
            .unwrap_or_default();
        quote! {(#types,)}
    };

    EnumField {
        name: field_name,
        as_type: as_type.clone(),
        selector: {
            let numbered = field
                .named
                .iter()
                .enumerate()
                .map(|(i, f)| {
                    let f = &f.ident;
                    let n =
                        syn::Ident::new(&format!("field_{}", i), proc_macro2::Span::call_site());
                    quote! {#f:#n,}
                })
                .fold(quote! {}, |acc, new| quote! {#acc #new});
            quote! {{#numbered}}
        },
        pack: {
            let numbered = field
                .named
                .iter()
                .enumerate()
                .map(|(i, _)| {
                    let f =
                        syn::Ident::new(&format!("field_{}", i), proc_macro2::Span::call_site());
                    quote! {#f,}
                })
                .fold(quote! {}, |acc, new| quote! {#acc #new});
            quote! {<#as_tuple_of_ref as #fracpack_mod::Pack>::pack(&(#numbered), dest)}
        },
        unpack: {
            let init = field
                .named
                .iter()
                .enumerate()
                .map(|(i, f)| {
                    let i = syn::Index::from(i);
                    let f = &f.ident;
                    quote! {#f: data.#i,}
                })
                .fold(quote! {}, |acc, new| quote! {#acc #new});
            quote! {
                {
                    let data = <#as_type as #fracpack_mod::Unpack>::unpack(src)?;
                    #enum_name::#field_name{#init}
                }
            }
        },
    }
}

fn enum_single<'a>(
    fracpack_mod: &proc_macro2::TokenStream,
    enum_name: &proc_macro2::Ident,
    field_name: &'a proc_macro2::Ident,
    field: &FieldsUnnamed,
) -> EnumField<'a> {
    let ty = &field.unnamed[0].ty;
    EnumField {
        name: field_name,
        as_type: {
            quote! {#ty}
        },
        selector: quote! {(field_0)},
        pack: quote! {<#ty as #fracpack_mod::Pack>::pack(field_0, dest)},
        unpack: quote! {
            #enum_name::#field_name(<#ty as #fracpack_mod::Unpack>::unpack(src)?)
        },
    }
}

fn enum_tuple<'a>(
    fracpack_mod: &proc_macro2::TokenStream,
    enum_name: &proc_macro2::Ident,
    field_name: &'a proc_macro2::Ident,
    field: &FieldsUnnamed,
) -> EnumField<'a> {
    let as_type = {
        let types = field
            .unnamed
            .iter()
            .map(|x| {
                let ty = &x.ty;
                quote! {#ty,}
            })
            .fold(quote! {}, |acc, new| quote! {#acc #new});
        quote! {(#types)}
    };
    let as_tuple_of_ref = {
        let types = field
            .unnamed
            .iter()
            .map(|x| {
                let ty = &x.ty;
                quote! {&#ty,}
            })
            .fold(quote! {}, |acc, new| quote! {#acc #new});
        quote! {(#types)}
    };

    EnumField {
        name: field_name,
        as_type: as_type.clone(),
        selector: {
            let numbered = field
                .unnamed
                .iter()
                .enumerate()
                .map(|(i, _)| {
                    let f =
                        syn::Ident::new(&format!("field_{}", i), proc_macro2::Span::call_site());
                    quote! {#f,}
                })
                .fold(quote! {}, |acc, new| quote! {#acc #new});
            quote! {(#numbered)}
        },
        pack: {
            let numbered = field
                .unnamed
                .iter()
                .enumerate()
                .map(|(i, _)| {
                    let f =
                        syn::Ident::new(&format!("field_{}", i), proc_macro2::Span::call_site());
                    quote! {#f,}
                })
                .fold(quote! {}, |acc, new| quote! {#acc #new});
            quote! {<#as_tuple_of_ref as #fracpack_mod::Pack>::pack(&(#numbered), dest)}
        },
        unpack: {
            let numbered = field
                .unnamed
                .iter()
                .enumerate()
                .map(|(i, _)| {
                    let i = syn::Index::from(i);
                    quote! {data.#i,}
                })
                .fold(quote! {}, |acc, new| quote! {#acc #new});
            quote! {
                {
                    let data = <#as_type as #fracpack_mod::Unpack>::unpack(src)?;
                    #enum_name::#field_name(#numbered)
                }
            }
        },
    }
}

fn enum_fields<'a>(
    fracpack_mod: &proc_macro2::TokenStream,
    enum_name: &proc_macro2::Ident,
    data: &'a DataEnum,
) -> Vec<EnumField<'a>> {
    data.variants
        .iter()
        .map(|var| {
            let field_name = &var.ident;

            match &var.fields {
                Fields::Named(field) => enum_named(fracpack_mod, enum_name, field_name, field),

                Fields::Unnamed(field) => {
                    if field.unnamed.len() == 1 {
                        enum_single(fracpack_mod, enum_name, field_name, field)
                    } else {
                        enum_tuple(fracpack_mod, enum_name, field_name, field)
                    }
                }
                Fields::Unit => unimplemented!("variants must have fields"), // TODO
            }
        })
        .collect()
}

pub fn fracpack_macro_impl(input: TokenStream, impl_pack: bool, impl_unpack: bool) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);

    let opts = match Options::from_derive_input(&input) {
        Ok(val) => val,
        Err(err) => {
            return err.write_errors().into();
        }
    };
    let fracpack_mod = proc_macro2::TokenStream::from_str(&opts.fracpack_mod).unwrap();

    match &input.data {
        Data::Struct(data) => {
            process_struct(&fracpack_mod, &input, impl_pack, impl_unpack, data, &opts)
        }
        Data::Enum(data) => process_enum(&fracpack_mod, &input, impl_pack, impl_unpack, data),
        Data::Union(_) => unimplemented!("fracpack does not support union"),
    }
}

// TODO: compile time: verify no non-optionals are after an optional
// TODO: unpack: check optionals not in heap
fn process_struct(
    fracpack_mod: &proc_macro2::TokenStream,
    input: &DeriveInput,
    impl_pack: bool,
    impl_unpack: bool,
    data: &DataStruct,
    opts: &Options,
) -> TokenStream {
    if let Fields::Unnamed(unnamed) = &data.fields {
        return process_struct_unnamed(fracpack_mod, input, impl_pack, impl_unpack, unnamed, opts);
    }
    let name = &input.ident;
    let generics = &input.generics;
    let (impl_generics, ty_generics, where_clause) = input.generics.split_for_impl();
    let fields = struct_fields(data);

    let used_fields: Vec<_> = fields.iter().filter(use_field).collect();
    let num_used_fields = used_fields.len();
    let last_field_ty = used_fields.last().map(|f| &f.ty);

    let optional_fields = fields.iter().filter(use_field).map(|field| {
        let ty = &field.ty;
        quote! {<#ty as #fracpack_mod::Pack>::IS_OPTIONAL}
    });

    let check_optional_fields: Vec<_> = fields.iter().filter(use_field).map(|field| {
        let name = &field.name;
        let ty = &field.ty;
        if opts.definition_will_not_change {
            quote! {true}
        } else {
            quote! {!<#ty as #fracpack_mod::Pack>::is_empty_optional(&self.#name)}
        }
    }).collect();

    let use_heap = if !opts.definition_will_not_change {
        quote! {true}
    } else {
        fields
            .iter()
            .filter(use_field)
            .map(|field| {
                let ty = &field.ty;
                quote! {<#ty as #fracpack_mod::Pack>::VARIABLE_SIZE}
            })
            .fold(quote! {false}, |acc, new| quote! {#acc || #new})
    };

    let fixed_size = fields
        .iter()
        .filter(use_field)
        .map(|field| {
            let ty = &field.ty;
            quote! {<#ty as #fracpack_mod::Pack>::FIXED_SIZE}
        })
        .fold(quote! {0}, |acc, new| quote! {#acc + #new});

    let fixed_data_size = fields
        .iter()
        .filter(use_field)
        .enumerate()
        .map(|(i, field)| {
            let ty = &field.ty;
            quote! { if trailing_empty_index > #i { <#ty as #fracpack_mod::Pack>::FIXED_SIZE } else { 0 } }
        })
        .fold(quote! {0}, |acc, new| quote! {#acc + #new});

    let positions: Vec<syn::Ident> = fields
        .iter()
        .filter(use_field)
        .map(|field| {
            let name = &field.name;
            let concatenated = format!("pos_{}", name);
            syn::Ident::new(&concatenated, name.span())
        })
        .collect();
    let pack_heap = if !opts.definition_will_not_change {
        quote! { <u16 as #fracpack_mod::Pack>::pack(&(heap as u16), dest); }
    } else {
        quote! {}
    };
    let unpack_heap_size = if !opts.definition_will_not_change {
        quote! {
            let fixed_size = <u16 as #fracpack_mod::Unpack>::unpack(src)?;
            let mut last_empty = false;
        }
    } else {
        quote! { let fixed_size = #fixed_size; }
    };

    let pack_fixed_members = fields
        .iter()
        .filter(use_field)
        .enumerate()
        .map(|(i, field)| {
            let name = &field.name;
            let ty = &field.ty;
            let pos = &positions[i];
            let pos_quote = quote! {
                #[allow(non_snake_case)]
                let #pos = dest.len() as u32;
            };
            if opts.definition_will_not_change {
                quote! {
                    #pos_quote
                    <#ty as #fracpack_mod::Pack>::embedded_fixed_pack(&self.#name, dest);
                }
            } else {
                quote! {
                    #pos_quote
                    if trailing_empty_index > #i {
                        <#ty as #fracpack_mod::Pack>::embedded_fixed_pack(&self.#name, dest);
                    }
                }
            }
        })
        .fold(quote! {}, |acc, new| quote! {#acc #new});

    let pack_variable_members = fields
        .iter()
        .filter(use_field)
        .enumerate()
        .map(|(i, field)| {
            let name = &field.name;
            let ty = &field.ty;
            let pos = &positions[i];
            if opts.definition_will_not_change {
                quote! {
                    <#ty as #fracpack_mod::Pack>::embedded_fixed_repack(&self.#name, #pos, dest.len() as u32, dest);
                    <#ty as #fracpack_mod::Pack>::embedded_variable_pack(&self.#name, dest);
                }
            } else {
                quote! {
                    if trailing_empty_index > #i {
                        <#ty as #fracpack_mod::Pack>::embedded_fixed_repack(&self.#name, #pos, dest.len() as u32, dest);
                        <#ty as #fracpack_mod::Pack>::embedded_variable_pack(&self.#name, dest);
                    }
                }
            }
        })
        .fold(quote! {}, |acc, new| quote! {#acc #new});

    let unpack = fields
        .iter()
        .filter(use_field)
        .enumerate()
        .map(|(i, field)| {
            let name = &field.name;
            let ty = &field.ty;
            if !opts.definition_will_not_change {
                quote! {
                    let #name = if #i < trailing_optional_index || fixed_pos < heap_pos {
                        last_empty = <#ty as #fracpack_mod::Unpack>::is_empty_optional(src, &mut fixed_pos.clone())?;
                        <#ty as #fracpack_mod::Unpack>::embedded_unpack(src, &mut fixed_pos)?
                    } else {
                        <#ty as #fracpack_mod::Unpack>::new_empty_optional()?
                    };
                }
            } else {
                quote! {
                    let #name = <#ty as #fracpack_mod::Unpack>::embedded_unpack(src, &mut fixed_pos)?;
                }
            }
        })
        .fold(quote! {}, |acc, new| quote! {#acc #new});

    let field_names_assignment = fields
        .iter()
        .map(|field| {
            let name = &field.name;
            if field.skip {
                quote! { #name: Default::default(), }
            } else {
                quote! {
                    #name,
                }
            }
        })
        .fold(quote! {}, |acc, new| quote! {#acc #new});

    // TODO: skip unknown members
    // TODO: option to verify no unknown members
    let verify = fields
        .iter()
        .filter(use_field)
        .enumerate()
        .map(|(i, field)| {
            let ty = &field.ty;
            // let name = &field.name;
            if !opts.definition_will_not_change {
                quote! {
                    if #i < trailing_optional_index || fixed_pos < heap_pos as u32 {
                        last_empty = <#ty as #fracpack_mod::Unpack>::is_empty_optional(src, &mut fixed_pos.clone())?;
                        <#ty as #fracpack_mod::Unpack>::embedded_verify(src, &mut fixed_pos)?;
                    }
                }
            } else {
                quote! {
                    <#ty as #fracpack_mod::Unpack>::embedded_verify(src, &mut fixed_pos)?;
                }
            }
        })
        .fold(quote! {}, |acc, new| quote! {#acc #new});

    let pack_impl = if impl_pack {
        if opts.definition_will_not_change {
            // definition_will_not_change: no trailing optional logic, no u16 heap header,
            // no per-field branching — all fields are always packed
            let packed_size_body = fields
                .iter()
                .filter(use_field)
                .map(|field| {
                    let name = &field.name;
                    let ty = &field.ty;
                    quote! {
                        + <#ty as #fracpack_mod::Pack>::FIXED_SIZE as usize
                        + <#ty as #fracpack_mod::Pack>::embedded_variable_packed_size(&self.#name)
                    }
                })
                .fold(quote! { 0usize }, |acc, new| quote! { #acc #new });

            quote! {
                impl #impl_generics #fracpack_mod::Pack for #name #ty_generics #where_clause {
                    const VARIABLE_SIZE: bool = #use_heap;
                    const FIXED_SIZE: u32 =
                        if <Self as #fracpack_mod::Pack>::VARIABLE_SIZE { 4 } else { #fixed_size };
                    fn pack(&self, dest: &mut Vec<u8>) {
                        #pack_fixed_members
                        #pack_variable_members
                    }
                    fn packed_size(&self) -> usize {
                        #packed_size_body
                    }
                }
            }
        } else {
            // Extensible struct: needs trailing optional detection and u16 heap header
            let packed_size_body = fields
                .iter()
                .filter(use_field)
                .enumerate()
                .map(|(i, field)| {
                    let name = &field.name;
                    let ty = &field.ty;
                    quote! {
                        + if trailing_empty_index > #i {
                            <#ty as #fracpack_mod::Pack>::FIXED_SIZE as usize
                            + <#ty as #fracpack_mod::Pack>::embedded_variable_packed_size(&self.#name)
                        } else { 0 }
                    }
                })
                .fold(quote! { 2usize }, |acc, new| quote! { #acc #new });

            // When the last field is non-optional, trailing_empty_index is always
            // num_fields. Emit a compile-time branch so the optimizer can constant-fold
            // the index, eliminate the array scan + rposition, and remove all per-field
            // `if trailing_empty_index > i` branches.
            let last_ty = last_field_ty.expect("extensible struct must have fields");
            let num = num_used_fields;

            quote! {
                impl #impl_generics #fracpack_mod::Pack for #name #ty_generics #where_clause {
                    const VARIABLE_SIZE: bool = #use_heap;
                    const FIXED_SIZE: u32 =
                        if <Self as #fracpack_mod::Pack>::VARIABLE_SIZE { 4 } else { #fixed_size };
                    fn pack(&self, dest: &mut Vec<u8>) {
                        let trailing_empty_index = if <#last_ty as #fracpack_mod::Pack>::IS_OPTIONAL {
                            [
                                #(#check_optional_fields),*
                            ].iter().rposition(|&is_non_empty| is_non_empty).map_or(0, |idx| idx + 1)
                        } else {
                            #num
                        };

                        let heap = #fixed_data_size;
                        assert!(heap as u16 as u32 == heap); // TODO: return error

                        #pack_heap
                        #pack_fixed_members
                        #pack_variable_members
                    }
                    fn packed_size(&self) -> usize {
                        let trailing_empty_index = if <#last_ty as #fracpack_mod::Pack>::IS_OPTIONAL {
                            [
                                #(#check_optional_fields),*
                            ].iter().rposition(|&is_non_empty| is_non_empty).map_or(0, |idx| idx + 1)
                        } else {
                            #num
                        };
                        #packed_size_body
                    }
                }
            }
        }
    } else {
        quote! {}
    };

    let unpack_last_non_optional_index = quote! {
        let optional_fields: Vec<bool> = vec![
            #(#optional_fields),*
        ];
        let trailing_optional_index = optional_fields.iter().rposition(|&is_optional| !is_optional).map_or(0, |idx| idx + 1);
    };

    let unpack_impl = if impl_unpack {
        let end_object = if !opts.definition_will_not_change {
            quote! {
                src.consume_trailing_optional(fixed_pos, heap_pos, last_empty)?;
            }
        } else {
            quote! {}
        };
        quote! {
            impl<'a> #fracpack_mod::Unpack<'a> for #name #generics {
                const VARIABLE_SIZE: bool = #use_heap;
                const FIXED_SIZE: u32 =
                    if <Self as #fracpack_mod::Unpack>::VARIABLE_SIZE { 4 } else { #fixed_size };
                fn unpack(src: &mut #fracpack_mod::FracInputStream<'a>) -> #fracpack_mod::Result<Self> {
                    #unpack_heap_size

                    #unpack_last_non_optional_index

                    let mut fixed_pos = src.advance(fixed_size as u32)?;
                    let heap_pos = src.pos;
                    #unpack
                    #end_object
                    let result = Self {
                        #field_names_assignment
                    };
                    Ok(result)
                }
                fn verify(src: &mut #fracpack_mod::FracInputStream) -> #fracpack_mod::Result<()> {
                    #unpack_heap_size

                    #unpack_last_non_optional_index

                    let mut fixed_pos = src.advance(fixed_size as u32)?;
                    let heap_pos = src.pos;
                    #verify
                    #end_object

                    Ok(())
                }
            }
        }
    } else {
        quote! {}
    };

    TokenStream::from(quote! {
        #pack_impl
        #unpack_impl
    })
} // process_struct

fn process_struct_unnamed(
    fracpack_mod: &proc_macro2::TokenStream,
    input: &DeriveInput,
    impl_pack: bool,
    impl_unpack: bool,
    unnamed: &FieldsUnnamed,
    opts: &Options,
) -> TokenStream {
    if opts.definition_will_not_change {
        unimplemented!("definition_will_not_change only supported on structs with named fields")
    }
    let name = &input.ident;
    let generics = &input.generics;
    let (impl_generics, ty_generics, where_clause) = input.generics.split_for_impl();

    let ty = if unnamed.unnamed.len() == 1 {
        let ty = &unnamed.unnamed[0].ty;
        quote! {#ty}
    } else {
        let ty = unnamed.unnamed.iter().fold(quote! {}, |acc, a| {
            let ty = &a.ty;
            quote! {#acc #ty,}
        });
        quote! {(#ty)}
    };
    let ty = quote! {<#ty as #fracpack_mod::Unpack>};

    let ref_ty = if unnamed.unnamed.len() == 1 {
        let ty = &unnamed.unnamed[0].ty;
        quote! {&#ty}
    } else {
        let ty = unnamed.unnamed.iter().fold(quote! {}, |acc, a| {
            let ty = &a.ty;
            quote! {#acc &#ty,}
        });
        quote! {(#ty)}
    };
    let ref_ty = quote! {<#ref_ty as #fracpack_mod::Pack>};

    let to_value = if unnamed.unnamed.len() == 1 {
        quote! {let value = &self.0;}
    } else {
        let to_value = unnamed
            .unnamed
            .iter()
            .enumerate()
            .fold(quote! {}, |acc, (i, _)| {
                let i = syn::Index::from(i);
                quote! {#acc &self.#i,}
            });
        quote! {let value = (#to_value);}
    };

    let from_value = if unnamed.unnamed.len() == 1 {
        quote! {Self(value)}
    } else {
        let from_value = unnamed
            .unnamed
            .iter()
            .enumerate()
            .fold(quote! {}, |acc, (i, _)| {
                let i = syn::Index::from(i);
                quote! {#acc value.#i,}
            });
        quote! {Self(#from_value)}
    };

    let (is_empty_container, new_empty_container) = if unnamed.unnamed.len() == 1 {
        (
            quote! {
                fn is_empty_container(&self) -> bool {
                    self.0.is_empty_container()
                }
            },
            quote! {
                fn new_empty_container() -> #fracpack_mod::Result<Self> {
                    Ok(Self(#ty::new_empty_container()?))
                }
                fn new_empty_optional() -> #fracpack_mod::Result<Self> {
                    Ok(Self(#ty::new_empty_optional()?))
                }
            },
        )
    } else {
        (quote! {}, quote! {})
    };

    let pack_impl = if impl_pack {
        quote! {
            impl #impl_generics #fracpack_mod::Pack for #name #ty_generics #where_clause {
                const FIXED_SIZE: u32 = #ty::FIXED_SIZE;
                const VARIABLE_SIZE: bool = #ty::VARIABLE_SIZE;
                const IS_OPTIONAL: bool = #ty::IS_OPTIONAL;

                fn pack(&self, dest: &mut Vec<u8>) {
                    #to_value
                    #ref_ty::pack(&value, dest)
                }

                #is_empty_container

                fn embedded_fixed_pack(&self, dest: &mut Vec<u8>) {
                    #to_value
                    #ref_ty::embedded_fixed_pack(&value, dest)
                }

                fn embedded_fixed_repack(&self, fixed_pos: u32, heap_pos: u32, dest: &mut Vec<u8>) {
                    #to_value
                    #ref_ty::embedded_fixed_repack(&value, fixed_pos, heap_pos, dest)
                }

                fn embedded_variable_pack(&self, dest: &mut Vec<u8>) {
                    #to_value
                    #ref_ty::embedded_variable_pack(&value, dest)
                }
            }
        }
    } else {
        quote! {}
    };

    let unpack_impl = if impl_unpack {
        quote! {
            impl<'a> #fracpack_mod::Unpack<'a> for #name #generics {
                const FIXED_SIZE: u32 = #ty::FIXED_SIZE;
                const VARIABLE_SIZE: bool = #ty::VARIABLE_SIZE;
                const IS_OPTIONAL: bool = #ty::IS_OPTIONAL;

                fn unpack(src: &mut #fracpack_mod::FracInputStream<'a>) -> #fracpack_mod::Result<Self> {
                    let value = #ty::unpack(src)?;
                    Ok(#from_value)
                }

                fn verify(src: &mut #fracpack_mod::FracInputStream) -> #fracpack_mod::Result<()> {
                    #ty::verify(src)
                }

                #new_empty_container

                fn embedded_variable_unpack(
                    src: &mut #fracpack_mod::FracInputStream<'a>,
                    fixed_pos: &mut u32,
                ) -> #fracpack_mod::Result<Self> {
                    let value = #ty::embedded_variable_unpack(src, fixed_pos)?;
                    Ok(#from_value)
                }

                fn embedded_unpack(src: &mut #fracpack_mod::FracInputStream<'a>, fixed_pos: &mut u32) -> #fracpack_mod::Result<Self> {
                    let value = #ty::embedded_unpack(src, fixed_pos)?;
                    Ok(#from_value)
                }

                fn embedded_variable_verify(
                    src: &mut #fracpack_mod::FracInputStream,
                    fixed_pos: &mut u32,
                ) -> #fracpack_mod::Result<()> {
                    #ty::embedded_variable_verify(src, fixed_pos)
                }

                fn embedded_verify(src: &mut #fracpack_mod::FracInputStream, fixed_pos: &mut u32) -> #fracpack_mod::Result<()> {
                    #ty::embedded_verify(src, fixed_pos)
                }
            }
        }
    } else {
        quote! {}
    };

    TokenStream::from(quote! {
        #pack_impl
        #unpack_impl
    })
}

fn process_enum(
    fracpack_mod: &proc_macro2::TokenStream,
    input: &DeriveInput,
    impl_pack: bool,
    impl_unpack: bool,
    data: &DataEnum,
) -> TokenStream {
    let name = &input.ident;
    let generics = &input.generics;
    let (impl_generics, ty_generics, where_clause) = input.generics.split_for_impl();
    let fields = enum_fields(fracpack_mod, name, data);
    // TODO: 128? also check during verify and unpack
    assert!(fields.len() < 256);
    let pack_items = fields
        .iter()
        .enumerate()
        .map(|(i, field)| {
            let index = i as u8;
            let field_name = &field.name;
            let selector = &field.selector;
            let pack = &field.pack;
            quote! {#name::#field_name #selector => {
                dest.push(#index);
                size_pos = dest.len();
                dest.extend_from_slice(&0_u32.to_le_bytes());
                #pack;
            }}
        })
        .fold(quote! {}, |acc, new| quote! {#acc #new});
    let unpack_items = fields
        .iter()
        .enumerate()
        .map(|(i, field)| {
            let index = i as u8;
            let unpack = &field.unpack;
            quote! {
                #index => #unpack,
            }
        })
        .fold(quote! {}, |acc, new| quote! {#acc #new});
    let verify_items = fields
        .iter()
        .enumerate()
        .map(|(i, field)| {
            let index = i as u8;
            let as_type = &field.as_type;
            quote! {
                #index => <#as_type as #fracpack_mod::Unpack>::verify(src)?,
            }
        })
        .fold(quote! {}, |acc, new| quote! {#acc #new});

    let pack_impl = if impl_pack {
        quote! {
            impl #impl_generics #fracpack_mod::Pack for #name #ty_generics #where_clause {
                const FIXED_SIZE: u32 = 4;
                const VARIABLE_SIZE: bool = true;
                fn pack(&self, dest: &mut Vec<u8>) {
                    let size_pos;
                    match &self {
                        #pack_items
                    };
                    let size = (dest.len() - size_pos - 4) as u32;
                    dest[size_pos..size_pos + 4].copy_from_slice(&size.to_le_bytes());
                }
            }
        }
    } else {
        quote! {}
    };

    let unpack_impl = if impl_unpack {
        quote! {
            impl<'a> #fracpack_mod::Unpack<'a> for #name #generics {
                const FIXED_SIZE: u32 = 4;
                const VARIABLE_SIZE: bool = true;
                fn unpack(outer: &mut #fracpack_mod::FracInputStream<'a>) -> #fracpack_mod::Result<Self> {
                    let index = <u8 as #fracpack_mod::Unpack>::unpack(outer)?;
                    let size = <u32 as #fracpack_mod::Unpack>::unpack(outer)?;
                    let mut inner = outer.read_fixed(size)?;
                    let src = &mut inner;
                    let result = match index {
                        #unpack_items
                        _ => return Err(#fracpack_mod::Error::BadEnumIndex),
                    };
                    if src.has_unknown {
                        outer.has_unknown = true;
                    }
                    src.finish()?;
                    Ok(result)
                }
                // TODO: option to error on unknown index
                fn verify(outer: &mut #fracpack_mod::FracInputStream) -> #fracpack_mod::Result<()> {
                    let index = <u8 as #fracpack_mod::Unpack>::unpack(outer)?;
                    let size = <u32 as #fracpack_mod::Unpack>::unpack(outer)?;
                    let mut inner = outer.read_fixed(size)?;
                    let src = &mut inner;
                    match index {
                        #verify_items
                        _ => return Err(#fracpack_mod::Error::BadEnumIndex),
                    }
                    if src.has_unknown {
                        outer.has_unknown = true;
                    }
                    src.finish()?;
                    Ok(())
                }
            }
        }
    } else {
        quote! {}
    };

    TokenStream::from(quote! {
        #pack_impl
        #unpack_impl
    })
} // process_enum

// ── FracView derive ──

pub fn fracview_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);

    let opts = match Options::from_derive_input(&input) {
        Ok(val) => val,
        Err(err) => {
            return err.write_errors().into();
        }
    };
    let fracpack_mod = proc_macro2::TokenStream::from_str(&opts.fracpack_mod).unwrap();

    match &input.data {
        Data::Struct(data) => process_struct_view(&fracpack_mod, &input, data, &opts),
        Data::Enum(data) => process_enum_view(&fracpack_mod, &input, data),
        Data::Union(_) => unimplemented!("fracpack view does not support union"),
    }
}

fn process_struct_view(
    fracpack_mod: &proc_macro2::TokenStream,
    input: &DeriveInput,
    data: &DataStruct,
    opts: &Options,
) -> TokenStream {
    if let Fields::Unnamed(_) = &data.fields {
        // Skip unnamed (tuple) structs for now
        return TokenStream::new();
    }

    let name = &input.ident;
    let vis = &input.vis;
    let view_name = syn::Ident::new(&format!("{}View", name), name.span());
    let fields = struct_fields(data);
    let active_fields: Vec<_> = fields.iter().filter(use_field).collect();

    // Generate view_to_owned field conversions (all fields, with defaults for skipped)
    let view_to_owned_fields: Vec<_> = fields.iter().map(|field| {
        let field_name = &field.name;
        let field_ty = &field.ty;
        if field.skip {
            quote! { #field_name: <#field_ty as Default>::default() }
        } else {
            quote! {
                #field_name: <#field_ty as #fracpack_mod::FracViewType>::view_to_owned(view.#field_name())
            }
        }
    }).collect();

    // Generate field accessor methods
    let field_accessors = active_fields.iter().enumerate().map(|(i, field)| {
        let field_name = &field.name;
        let field_ty = &field.ty;

        // Offset = sum of preceding field FIXED_SIZEs
        let preceding_sizes: Vec<_> = active_fields[..i]
            .iter()
            .map(|f| {
                let ty = &f.ty;
                quote! { <#ty as #fracpack_mod::Pack>::FIXED_SIZE }
            })
            .collect();

        let offset_expr = if preceding_sizes.is_empty() {
            quote! { 0u32 }
        } else {
            quote! { 0u32 #(+ #preceding_sizes)* }
        };

        if opts.definition_will_not_change {
            // No trailing optional elision possible
            quote! {
                pub fn #field_name(&self) -> <#field_ty as #fracpack_mod::FracViewType<'a>>::View {
                    let mut pos = self.base + #offset_expr;
                    <#field_ty as #fracpack_mod::FracViewType<'a>>::view_embedded(self.data, &mut pos)
                }
            }
        } else {
            // Need trailing optional elision check
            let all_is_optional = active_fields.iter().map(|f| {
                let ty = &f.ty;
                quote! { <#ty as #fracpack_mod::Pack>::IS_OPTIONAL }
            });

            quote! {
                pub fn #field_name(&self) -> <#field_ty as #fracpack_mod::FracViewType<'a>>::View {
                    let field_offset: u32 = #offset_expr;
                    const TRAILING_OPT_IDX: usize = {
                        let flags: &[bool] = &[#(#all_is_optional),*];
                        let mut i = flags.len();
                        loop {
                            if i == 0 { break 0; }
                            i -= 1;
                            if !flags[i] { break i + 1; }
                        }
                    };
                    if #i >= TRAILING_OPT_IDX && field_offset >= self.heap_size as u32 {
                        return <#field_ty as #fracpack_mod::FracViewType<'a>>::view_empty();
                    }
                    let mut pos = self.base + field_offset;
                    <#field_ty as #fracpack_mod::FracViewType<'a>>::view_embedded(self.data, &mut pos)
                }
            }
        }
    });

    // Generate Debug impl
    let debug_fields = active_fields.iter().map(|field| {
        let field_name = &field.name;
        let field_name_str = field_name.to_string();
        quote! { .field(#field_name_str, &self.#field_name()) }
    });
    let name_str = name.to_string();

    // Generate struct definition and impls based on extensibility
    if opts.definition_will_not_change {
        // No heap_size field needed
        let use_heap = active_fields
            .iter()
            .map(|f| {
                let ty = &f.ty;
                quote! { <#ty as #fracpack_mod::Pack>::VARIABLE_SIZE }
            })
            .fold(quote! { false }, |acc, new| quote! { #acc || #new });

        let fixed_size = active_fields
            .iter()
            .map(|f| {
                let ty = &f.ty;
                quote! { <#ty as #fracpack_mod::Pack>::FIXED_SIZE }
            })
            .fold(quote! { 0u32 }, |acc, new| quote! { #acc + #new });

        TokenStream::from(quote! {
            #[derive(Clone, Copy)]
            #vis struct #view_name<'a> {
                data: &'a [u8],
                base: u32,
            }

            impl<'a> #view_name<'a> {
                #(#field_accessors)*
            }

            impl<'a> std::fmt::Debug for #view_name<'a> {
                fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                    f.debug_struct(#name_str)
                        #(#debug_fields)*
                        .finish()
                }
            }

            impl<'a> #fracpack_mod::FracViewType<'a> for #name {
                type View = #view_name<'a>;

                fn view_at(data: &'a [u8], pos: u32) -> #view_name<'a> {
                    #view_name { data, base: pos }
                }

                fn view_embedded(data: &'a [u8], fixed_pos: &mut u32) -> #view_name<'a> {
                    const VAR: bool = #use_heap;
                    if VAR {
                        let p = *fixed_pos as usize;
                        let offset = u32::from_le_bytes(
                            data[p..p + 4].try_into().unwrap(),
                        );
                        *fixed_pos += 4;
                        Self::view_at(data, p as u32 + offset)
                    } else {
                        let base = *fixed_pos;
                        const FSIZE: u32 = #fixed_size;
                        *fixed_pos += FSIZE;
                        #view_name { data, base }
                    }
                }

                fn view_to_owned(view: #view_name<'a>) -> #name {
                    #name {
                        #(#view_to_owned_fields),*
                    }
                }
            }
        })
    } else {
        // Extensible struct: needs heap_size
        TokenStream::from(quote! {
            #[derive(Clone, Copy)]
            #vis struct #view_name<'a> {
                data: &'a [u8],
                base: u32,
                heap_size: u16,
            }

            impl<'a> #view_name<'a> {
                #(#field_accessors)*
            }

            impl<'a> std::fmt::Debug for #view_name<'a> {
                fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                    f.debug_struct(#name_str)
                        #(#debug_fields)*
                        .finish()
                }
            }

            impl<'a> #fracpack_mod::FracViewType<'a> for #name {
                type View = #view_name<'a>;

                fn view_at(data: &'a [u8], pos: u32) -> #view_name<'a> {
                    let p = pos as usize;
                    let heap_size = u16::from_le_bytes(
                        data[p..p + 2].try_into().unwrap(),
                    );
                    #view_name { data, base: pos + 2, heap_size }
                }

                fn view_embedded(data: &'a [u8], fixed_pos: &mut u32) -> #view_name<'a> {
                    let p = *fixed_pos as usize;
                    let offset = u32::from_le_bytes(
                        data[p..p + 4].try_into().unwrap(),
                    );
                    *fixed_pos += 4;
                    Self::view_at(data, p as u32 + offset)
                }

                fn view_to_owned(view: #view_name<'a>) -> #name {
                    #name {
                        #(#view_to_owned_fields),*
                    }
                }
            }
        })
    }
}

fn process_enum_view(
    fracpack_mod: &proc_macro2::TokenStream,
    input: &DeriveInput,
    data: &DataEnum,
) -> TokenStream {
    let name = &input.ident;
    let vis = &input.vis;
    let view_name = syn::Ident::new(&format!("{}View", name), name.span());

    // Build variant info
    let variants: Vec<_> = data
        .variants
        .iter()
        .enumerate()
        .map(|(i, var)| {
            let var_name = &var.ident;
            let index = i as u8;
            match &var.fields {
                Fields::Unnamed(unnamed) if unnamed.unnamed.len() == 1 => {
                    let ty = &unnamed.unnamed[0].ty;
                    (var_name, ty, index)
                }
                _ => unimplemented!(
                    "FracView only supports enum variants with a single unnamed field"
                ),
            }
        })
        .collect();

    // Generate view enum variants
    let view_variants = variants.iter().map(|(var_name, ty, _)| {
        quote! { #var_name(<#ty as #fracpack_mod::FracViewType<'a>>::View) }
    });

    // Generate match arms for view_at
    let view_at_arms = variants.iter().map(|(var_name, ty, index)| {
        quote! {
            #index => #view_name::#var_name(
                <#ty as #fracpack_mod::FracViewType<'a>>::view_at(data, data_pos)
            )
        }
    });

    // Generate Debug match arms
    let debug_arms = variants.iter().map(|(var_name, _, _)| {
        let var_str = var_name.to_string();
        quote! {
            #view_name::#var_name(v) => f.debug_tuple(#var_str).field(v).finish()
        }
    });

    // Generate view_to_owned match arms
    let to_owned_arms = variants.iter().map(|(var_name, ty, _)| {
        quote! {
            #view_name::#var_name(v) => #name::#var_name(
                <#ty as #fracpack_mod::FracViewType>::view_to_owned(v)
            )
        }
    });

    TokenStream::from(quote! {
        #[derive(Clone, Copy)]
        #vis enum #view_name<'a> {
            #(#view_variants),*
        }

        impl<'a> std::fmt::Debug for #view_name<'a> {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                match self {
                    #(#debug_arms),*
                }
            }
        }

        impl<'a> #fracpack_mod::FracViewType<'a> for #name {
            type View = #view_name<'a>;

            fn view_at(data: &'a [u8], pos: u32) -> #view_name<'a> {
                let p = pos as usize;
                let tag = data[p];
                let data_pos = pos + 5; // skip u8 tag + u32 size
                match tag {
                    #(#view_at_arms,)*
                    _ => panic!("invalid enum tag in validated data"),
                }
            }

            fn view_embedded(data: &'a [u8], fixed_pos: &mut u32) -> #view_name<'a> {
                let p = *fixed_pos as usize;
                let offset = u32::from_le_bytes(
                    data[p..p + 4].try_into().unwrap(),
                );
                *fixed_pos += 4;
                Self::view_at(data, p as u32 + offset)
            }

            fn view_to_owned(view: #view_name<'a>) -> #name {
                match view {
                    #(#to_owned_arms),*
                }
            }
        }
    })
}

// ── FracMutView derive ──

pub fn fracmutview_macro_impl(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);

    let opts = match Options::from_derive_input(&input) {
        Ok(val) => val,
        Err(err) => {
            return err.write_errors().into();
        }
    };
    let fracpack_mod = proc_macro2::TokenStream::from_str(&opts.fracpack_mod).unwrap();

    match &input.data {
        Data::Struct(data) => process_struct_mutview(&fracpack_mod, &input, data, &opts),
        Data::Enum(data) => process_enum_mutview(&fracpack_mod, &input, data),
        Data::Union(_) => unimplemented!("FracMutView does not support union"),
    }
}

fn field_offset_expr(
    fracpack_mod: &proc_macro2::TokenStream,
    active_fields: &[&StructField],
    field_index: usize,
) -> proc_macro2::TokenStream {
    let preceding: Vec<_> = active_fields[..field_index]
        .iter()
        .map(|f| {
            let ty = &f.ty;
            quote! { <#ty as #fracpack_mod::Pack>::FIXED_SIZE }
        })
        .collect();
    if preceding.is_empty() {
        quote! { 0u32 }
    } else {
        quote! { 0u32 #(+ #preceding)* }
    }
}

fn process_struct_mutview(
    fracpack_mod: &proc_macro2::TokenStream,
    input: &DeriveInput,
    data: &DataStruct,
    opts: &Options,
) -> TokenStream {
    if let Fields::Unnamed(_) = &data.fields {
        return TokenStream::new();
    }

    let name = &input.ident;
    let vis = &input.vis;
    let mut_base_name = syn::Ident::new(&format!("{}MutBase", name), name.span());
    let mut_name = syn::Ident::new(&format!("{}Mut", name), name.span());
    let mut_canonical_name = syn::Ident::new(&format!("{}MutCanonical", name), name.span());
    let fields = struct_fields(data);
    let active_fields: Vec<_> = fields.iter().filter(use_field).collect();

    let is_extensible = !opts.definition_will_not_change;

    // ── Getters ──
    let getters = active_fields.iter().enumerate().map(|(i, field)| {
        let field_name = &field.name;
        let field_ty = &field.ty;
        let offset_expr = field_offset_expr(fracpack_mod, &active_fields, i);

        if is_extensible {
            let all_is_optional = active_fields.iter().map(|f| {
                let ty = &f.ty;
                quote! { <#ty as #fracpack_mod::Pack>::IS_OPTIONAL }
            });
            quote! {
                pub fn #field_name<'a>(&self, data: &'a [u8]) -> <#field_ty as #fracpack_mod::FracViewType<'a>>::View {
                    let field_offset: u32 = #offset_expr;
                    let trailing_opt_idx = [#(#all_is_optional),*]
                        .iter()
                        .rposition(|&x| !x)
                        .map_or(0, |idx| idx + 1);
                    if #i >= trailing_opt_idx && field_offset >= self.heap_size as u32 {
                        return <#field_ty as #fracpack_mod::FracViewType<'a>>::view_empty();
                    }
                    let mut pos = self.data_base + field_offset;
                    <#field_ty as #fracpack_mod::FracViewType<'a>>::view_embedded(data, &mut pos)
                }
            }
        } else {
            quote! {
                pub fn #field_name<'a>(&self, data: &'a [u8]) -> <#field_ty as #fracpack_mod::FracViewType<'a>>::View {
                    let mut pos = self.data_base + #offset_expr;
                    <#field_ty as #fracpack_mod::FracViewType<'a>>::view_embedded(data, &mut pos)
                }
            }
        }
    });

    // ── Setters (dispatch on FAST const generic) ──
    let setters = active_fields.iter().enumerate().map(|(i, field)| {
        let field_name = &field.name;
        let set_name = syn::Ident::new(&format!("set_{}", field_name), field_name.span());
        let field_ty = &field.ty;
        let offset_expr = field_offset_expr(fracpack_mod, &active_fields, i);

        // Canonical-mode patch calls for all variable-size field offsets
        let patch_calls: Vec<_> = active_fields
            .iter()
            .enumerate()
            .map(|(j, f)| {
                let ty = &f.ty;
                let f_offset = field_offset_expr(fracpack_mod, &active_fields, j);
                if is_extensible {
                    quote! {
                        {
                            let fo: u32 = #f_offset;
                            if <#ty as #fracpack_mod::Pack>::VARIABLE_SIZE
                                && fo + <#ty as #fracpack_mod::Pack>::FIXED_SIZE <= self.heap_size as u32
                            {
                                #fracpack_mod::patch_offset(data, self.data_base + fo, after_old, delta);
                            }
                        }
                    }
                } else {
                    quote! {
                        if <#ty as #fracpack_mod::Pack>::VARIABLE_SIZE {
                            #fracpack_mod::patch_offset(data, self.data_base + #f_offset, after_old, delta);
                        }
                    }
                }
            })
            .collect();

        let elision_check = if is_extensible {
            quote! {
                assert!(
                    #offset_expr + <#field_ty as #fracpack_mod::Pack>::FIXED_SIZE <= self.heap_size as u32,
                    "FracMutView: cannot set elided trailing optional field"
                );
            }
        } else {
            quote! {}
        };

        quote! {
            pub fn #set_name(&self, data: &mut Vec<u8>, value: &#field_ty) {
                #elision_check
                let field_pos = self.data_base + #offset_expr;
                if FAST {
                    <#field_ty as #fracpack_mod::FracMutViewType>::embedded_replace_fast(data, field_pos, value);
                } else {
                    let (after_old, delta) = <#field_ty as #fracpack_mod::FracMutViewType>::embedded_replace(data, field_pos, value);
                    if delta != 0 {
                        #(#patch_calls)*
                    }
                }
            }
        }
    });

    // ── compact field conversions (all fields, with defaults for skipped) ──
    let compact_fields: Vec<_> = fields.iter().map(|field| {
        let field_name = &field.name;
        let field_ty = &field.ty;
        if field.skip {
            quote! { #field_name: <#field_ty as Default>::default() }
        } else {
            quote! {
                #field_name: <#field_ty as #fracpack_mod::FracViewType>::view_to_owned(m.#field_name(data))
            }
        }
    }).collect();

    // ── packed_size_at body ──
    let size_fields: Vec<_> = active_fields
        .iter()
        .enumerate()
        .map(|(j, f)| {
            let ty = &f.ty;
            let f_offset = field_offset_expr(fracpack_mod, &active_fields, j);
            if is_extensible {
                quote! {
                    {
                        let fo: u32 = #f_offset;
                        if <#ty as #fracpack_mod::Pack>::VARIABLE_SIZE
                            && fo + <#ty as #fracpack_mod::Pack>::FIXED_SIZE <= heap_size
                        {
                            let field_end = <#ty as #fracpack_mod::FracMutViewType>::embedded_data_end(data, data_base + fo);
                            if field_end > end { end = field_end; }
                        }
                    }
                }
            } else {
                quote! {
                    if <#ty as #fracpack_mod::Pack>::VARIABLE_SIZE {
                        let fo: u32 = #f_offset;
                        let field_end = <#ty as #fracpack_mod::FracMutViewType>::embedded_data_end(data, data_base + fo);
                        if field_end > end { end = field_end; }
                    }
                }
            }
        })
        .collect();

    let fixed_size_total = active_fields
        .iter()
        .map(|f| {
            let ty = &f.ty;
            quote! { <#ty as #fracpack_mod::Pack>::FIXED_SIZE }
        })
        .fold(quote! { 0u32 }, |acc, new| quote! { #acc + #new });

    if is_extensible {
        TokenStream::from(quote! {
            /// Mutable view for in-place mutation of packed data.
            /// `FAST=true`: overwrites in place / appends (non-canonical, call `compact()` when done).
            /// `FAST=false`: splice-and-patch (immediately canonical).
            #[derive(Clone, Copy)]
            #vis struct #mut_base_name<const FAST: bool> {
                data_base: u32,
                heap_size: u16,
            }

            /// Fast mutable view — O(1) shrinks, O(new_size) grows, non-canonical.
            #vis type #mut_name = #mut_base_name<true>;
            /// Canonical mutable view — splice-and-patch, result is always canonical.
            #vis type #mut_canonical_name = #mut_base_name<false>;

            impl<const FAST: bool> #mut_base_name<FAST> {
                /// Create a mutable view of top-level packed data. Validates first.
                pub fn new(data: &[u8]) -> #fracpack_mod::Result<Self> {
                    <#name as #fracpack_mod::Unpack>::verify_no_extra(data)?;
                    let heap_size = u16::from_le_bytes(data[0..2].try_into().unwrap());
                    Ok(Self { data_base: 2, heap_size })
                }

                /// Create from a known position (for nested structs). No validation.
                pub fn at(data: &[u8], pos: u32) -> Self {
                    let p = pos as usize;
                    let heap_size = u16::from_le_bytes(data[p..p + 2].try_into().unwrap());
                    Self { data_base: pos + 2, heap_size }
                }

                #(#getters)*
                #(#setters)*

                /// Restore canonical packed form after non-canonical mutations.
                pub fn compact(data: &mut Vec<u8>) {
                    let heap_size = u16::from_le_bytes(data[0..2].try_into().unwrap());
                    let m = Self { data_base: 2, heap_size };
                    let val = #name {
                        #(#compact_fields),*
                    };
                    *data = <#name as #fracpack_mod::Pack>::packed(&val);
                }
            }

            impl #fracpack_mod::FracMutViewType for #name {
                fn packed_size_at(data: &[u8], pos: u32) -> u32 {
                    let p = pos as usize;
                    let heap_size = u16::from_le_bytes(data[p..p + 2].try_into().unwrap()) as u32;
                    let data_base = pos + 2;
                    let mut end = data_base + heap_size;
                    #(#size_fields)*
                    end - pos
                }
            }
        })
    } else {
        // definition_will_not_change
        let use_heap = active_fields
            .iter()
            .map(|f| {
                let ty = &f.ty;
                quote! { <#ty as #fracpack_mod::Pack>::VARIABLE_SIZE }
            })
            .fold(quote! { false }, |acc, new| quote! { #acc || #new });

        TokenStream::from(quote! {
            /// Mutable view for in-place mutation of packed data.
            /// `FAST=true`: overwrites in place / appends (non-canonical, call `compact()` when done).
            /// `FAST=false`: splice-and-patch (immediately canonical).
            #[derive(Clone, Copy)]
            #vis struct #mut_base_name<const FAST: bool> {
                data_base: u32,
            }

            /// Fast mutable view — O(1) shrinks, O(new_size) grows, non-canonical.
            #vis type #mut_name = #mut_base_name<true>;
            /// Canonical mutable view — splice-and-patch, result is always canonical.
            #vis type #mut_canonical_name = #mut_base_name<false>;

            impl<const FAST: bool> #mut_base_name<FAST> {
                /// Create a mutable view of top-level packed data. Validates first.
                pub fn new(data: &[u8]) -> #fracpack_mod::Result<Self> {
                    <#name as #fracpack_mod::Unpack>::verify_no_extra(data)?;
                    Ok(Self { data_base: 0 })
                }

                /// Create from a known position (for nested structs). No validation.
                pub fn at(_data: &[u8], pos: u32) -> Self {
                    Self { data_base: pos }
                }

                #(#getters)*
                #(#setters)*

                /// Restore canonical packed form after non-canonical mutations.
                pub fn compact(data: &mut Vec<u8>) {
                    let m = Self { data_base: 0 };
                    let val = #name {
                        #(#compact_fields),*
                    };
                    *data = <#name as #fracpack_mod::Pack>::packed(&val);
                }
            }

            impl #fracpack_mod::FracMutViewType for #name {
                fn packed_size_at(data: &[u8], pos: u32) -> u32 {
                    let data_base = pos;
                    let fixed_total: u32 = #fixed_size_total;
                    let mut end = data_base + fixed_total;
                    const VAR: bool = #use_heap;
                    if VAR {
                        #(#size_fields)*
                    }
                    end - pos
                }
            }
        })
    }
}

fn process_enum_mutview(
    fracpack_mod: &proc_macro2::TokenStream,
    input: &DeriveInput,
    _data: &DataEnum,
) -> TokenStream {
    let name = &input.ident;

    TokenStream::from(quote! {
        impl #fracpack_mod::FracMutViewType for #name {
            fn packed_size_at(data: &[u8], pos: u32) -> u32 {
                let p = pos as usize;
                let size = u32::from_le_bytes(data[p + 1..p + 5].try_into().unwrap());
                5 + size
            }
        }
    })
}
