//! The helper attributes every PSIO derive reads, parsed in one place.
//!
//! `#[psio(...)]` describes the type and is read by every derive. A
//! format-named attribute holds only what is specific to that format:
//!
//! | attribute | key | site | meaning |
//! |-----------|-----|------|---------|
//! | `psio` | `crate = "path"` | item | path of the psio crate (default `::psio`) |
//! | `psio` | `transparent` | item | the type is its one member in every format |
//! | `psio` | `invariant = "path"` | item | `fn(&Self) -> Result<(), &'static str>` every reader and writer enforces |
//! | `psio` | `definition_will_not_change` | item | the record never gains members |
//! | `psio` | `variant` | item | a field-less enum is a variant of unit alternatives, not its integer |
//! | `psio` | `custom`, `custom = "id"` | item | the type has a custom text form; the id defaults to the schema name |
//! | `psio` | `name = "..."` | item, variant | the name of the type in a schema; the name of an enumerator or alternative in a schema and in text |
//! | `psio` | `other` | variant | the catch-all of a field-less `#[repr(int)]` enum |
//! | `pssz` | `max_dynamic_data = N` | item | pSSZ cap on the record's variable part |
//! | `json` | `json_number`, `json_string`, `ijson_number`, `ijson_string` | item, field | integer form in Json / IJson text |
//! | `frac` | `skip` | field | the experimental frac derives leave the member out |
//!
//! `#[serde(transparent)]` on the item means `#[psio(transparent)]`.
//!
//! A key under the wrong attribute, at the wrong site, or unknown is an
//! error. Every derive checks every attribute in this table that it can see,
//! so a misplaced key is reported whichever derives the type uses.

use syn::{Attribute, DeriveInput, Lit, Meta, NestedMeta};

/// The attributes parsed here, in table order.
const NAMESPACES: [&str; 4] = ["psio", "pssz", "json", "frac"];

#[derive(Clone, Copy, PartialEq)]
pub(crate) enum Site {
    Item,
    Field,
    Variant,
}

/// One row of the table: the key, the attribute it belongs to, the sites it
/// applies to, why it lives there, and how it is written.
struct Key {
    name: &'static str,
    namespace: &'static str,
    sites: &'static [Site],
    because: &'static str,
    usage: &'static str,
}

const KEYS: &[Key] = &[
    Key { name: "crate", namespace: "psio", sites: &[Site::Item],
          because: "names the psio crate for every derive", usage: "crate = \"path\"" },
    Key { name: "transparent", namespace: "psio", sites: &[Site::Item],
          because: "describes the type for every format", usage: "transparent" },
    Key { name: "invariant", namespace: "psio", sites: &[Site::Item],
          because: "describes the type for every format", usage: "invariant = \"path\"" },
    Key { name: "definition_will_not_change", namespace: "psio", sites: &[Site::Item],
          because: "describes the type for every format", usage: "definition_will_not_change" },
    Key { name: "variant", namespace: "psio", sites: &[Site::Item],
          because: "describes the type for every format", usage: "variant" },
    Key { name: "custom", namespace: "psio", sites: &[Site::Item],
          because: "describes the type for every format", usage: "custom, custom = \"id\"" },
    Key { name: "name", namespace: "psio", sites: &[Site::Item, Site::Variant],
          because: "names the type or alternative for every format", usage: "name = \"...\"" },
    Key { name: "other", namespace: "psio", sites: &[Site::Variant],
          because: "describes the enum for every format", usage: "other" },
    Key { name: "max_dynamic_data", namespace: "pssz", sites: &[Site::Item],
          because: "is a pSSZ wire limit", usage: "max_dynamic_data = N" },
    Key { name: "json_number", namespace: "json", sites: &[Site::Item, Site::Field],
          because: "is a JSON text form", usage: "json_number" },
    Key { name: "json_string", namespace: "json", sites: &[Site::Item, Site::Field],
          because: "is a JSON text form", usage: "json_string" },
    Key { name: "ijson_number", namespace: "json", sites: &[Site::Item, Site::Field],
          because: "is a JSON text form", usage: "ijson_number" },
    Key { name: "ijson_string", namespace: "json", sites: &[Site::Item, Site::Field],
          because: "is a JSON text form", usage: "ijson_string" },
    Key { name: "skip", namespace: "frac", sites: &[Site::Field],
          because: "is read by the experimental frac derives only", usage: "skip" },
];

/// Integer form in Json / IJson text: `Some(true)` is a string, `Some(false)`
/// a number, `None` the type's natural form.
#[derive(Clone, Copy, Default)]
pub(crate) struct Forms {
    pub(crate) json: Option<bool>,
    pub(crate) ijson: Option<bool>,
}

/// Every key of the table, as written on one item, field or variant.
#[derive(Default)]
pub(crate) struct Attrs {
    /// `#[psio(crate = "path")]`.
    pub(crate) krate: Option<syn::Path>,
    /// `#[psio(transparent)]` or `#[serde(transparent)]`.
    pub(crate) transparent: bool,
    /// `#[psio(invariant = "path")]`.
    pub(crate) invariant: Option<syn::Path>,
    /// `#[psio(definition_will_not_change)]`.
    pub(crate) definition_will_not_change: bool,
    /// `#[psio(variant)]`.
    pub(crate) variant: bool,
    /// `#[psio(custom)]` is `Some(None)`, `#[psio(custom = "id")]` is
    /// `Some(Some(id))`.
    pub(crate) custom: Option<Option<String>>,
    /// `#[psio(name = "...")]`.
    pub(crate) name: Option<String>,
    /// `#[psio(other)]`.
    pub(crate) other: bool,
    /// `#[pssz(max_dynamic_data = N)]`.
    pub(crate) max_dynamic_data: Option<u64>,
    /// `#[json(json_number)]` and the other three integer forms.
    pub(crate) forms: Forms,
    /// `#[frac(skip)]`.
    pub(crate) skip: bool,
}

impl Attrs {
    /// The psio crate: `#[psio(crate = "path")]`, else `::psio`.
    pub(crate) fn krate(&self) -> syn::Path {
        self.krate.clone().unwrap_or_else(|| syn::parse_quote!(::psio))
    }
}

fn site_name(site: Site) -> &'static str {
    match site {
        Site::Item => "the type",
        Site::Field => "a field",
        Site::Variant => "an enum variant",
    }
}

fn sites_text(sites: &[Site]) -> String {
    sites.iter().map(|s| site_name(*s)).collect::<Vec<_>>().join(" or ")
}

fn accepted(namespace: &str, site: Site) -> String {
    let keys: Vec<_> = KEYS
        .iter()
        .filter(|k| k.namespace == namespace && k.sites.contains(&site))
        .map(|k| k.usage)
        .collect();
    if keys.is_empty() {
        format!("#[{namespace}(...)] has no keys for {}", site_name(site))
    } else {
        format!("#[{namespace}(...)] on {} accepts: {}", site_name(site), keys.join(", "))
    }
}

fn string(nv: &syn::MetaNameValue, what: &str) -> syn::Result<syn::LitStr> {
    match &nv.lit {
        Lit::Str(value) => Ok(value.clone()),
        _ => Err(syn::Error::new_spanned(nv, format!("{what}: {} = \"...\"", path_text(&nv.path)))),
    }
}

fn path_text(path: &syn::Path) -> String {
    path.segments.iter().map(|s| s.ident.to_string()).collect::<Vec<_>>().join("::")
}

fn set_flag(slot: &mut bool, meta: &Meta) -> syn::Result<()> {
    match meta {
        Meta::Path(_) => {
            *slot = true;
            Ok(())
        }
        _ => Err(syn::Error::new_spanned(
            meta,
            format!("`{}` takes no value", path_text(meta.path())),
        )),
    }
}

fn set_form(slot: &mut Option<bool>, string_form: bool, meta: &Meta) -> syn::Result<()> {
    let mut flag = false;
    set_flag(&mut flag, meta)?;
    *slot = Some(string_form);
    Ok(())
}

fn name_value<'a>(meta: &'a Meta, usage: &str) -> syn::Result<&'a syn::MetaNameValue> {
    match meta {
        Meta::NameValue(nv) => Ok(nv),
        _ => Err(syn::Error::new_spanned(meta, format!("expected {usage}"))),
    }
}

fn store(out: &mut Attrs, key: &Key, meta: &Meta) -> syn::Result<()> {
    match key.name {
        "crate" => {
            out.krate = Some(string(name_value(meta, key.usage)?, "crate is a path string")?.parse()?)
        }
        "transparent" => set_flag(&mut out.transparent, meta)?,
        "invariant" => {
            if out.invariant.is_some() {
                return Err(syn::Error::new_spanned(meta, "a type has one invariant"));
            }
            out.invariant =
                Some(string(name_value(meta, key.usage)?, "invariant is a path string")?.parse()?)
        }
        "definition_will_not_change" => set_flag(&mut out.definition_will_not_change, meta)?,
        "variant" => set_flag(&mut out.variant, meta)?,
        "custom" => {
            out.custom = Some(match meta {
                Meta::Path(_) => None,
                Meta::NameValue(nv) => Some(string(nv, "custom is a string")?.value()),
                _ => return Err(syn::Error::new_spanned(meta, "expected custom or custom = \"id\"")),
            })
        }
        "name" => out.name = Some(string(name_value(meta, key.usage)?, "name is a string")?.value()),
        "other" => set_flag(&mut out.other, meta)?,
        "max_dynamic_data" => {
            let nv = name_value(meta, key.usage)?;
            let Lit::Int(value) = &nv.lit else {
                return Err(syn::Error::new_spanned(nv, "max_dynamic_data is an integer"));
            };
            out.max_dynamic_data = Some(value.base10_parse()?);
        }
        "json_number" => set_form(&mut out.forms.json, false, meta)?,
        "json_string" => set_form(&mut out.forms.json, true, meta)?,
        "ijson_number" => set_form(&mut out.forms.ijson, false, meta)?,
        "ijson_string" => set_form(&mut out.forms.ijson, true, meta)?,
        "skip" => set_flag(&mut out.skip, meta)?,
        _ => unreachable!("every key of the table is stored"),
    }
    Ok(())
}

/// `#[serde(transparent)]`: a type that is its one member for serde is its
/// one member here.
fn serde_transparent(attr: &Attribute) -> bool {
    if !attr.path.is_ident("serde") {
        return false;
    }
    match attr.parse_meta() {
        Ok(Meta::List(list)) => list.nested.iter().any(
            |item| matches!(item, NestedMeta::Meta(Meta::Path(path)) if path.is_ident("transparent")),
        ),
        _ => false,
    }
}

/// The table's keys on one item, field or variant.
pub(crate) fn parse(attrs: &[Attribute], site: Site) -> syn::Result<Attrs> {
    let mut out = Attrs::default();
    for attr in attrs {
        if site == Site::Item && serde_transparent(attr) {
            out.transparent = true;
        }
        let Some(namespace) = NAMESPACES.iter().copied().find(|n| attr.path.is_ident(n)) else {
            continue;
        };
        let list = match attr.parse_meta()? {
            Meta::List(list) => list,
            other => {
                return Err(syn::Error::new_spanned(other, format!("expected #[{namespace}(...)]")))
            }
        };
        for item in &list.nested {
            let NestedMeta::Meta(meta) = item else {
                return Err(syn::Error::new_spanned(item, accepted(namespace, site)));
            };
            let written = path_text(meta.path());
            let Some(key) = KEYS.iter().find(|k| meta.path().is_ident(k.name)) else {
                return Err(syn::Error::new_spanned(
                    meta,
                    format!("unknown key `{written}`: {}", accepted(namespace, site)),
                ));
            };
            if key.namespace != namespace {
                return Err(syn::Error::new_spanned(
                    meta,
                    format!(
                        "`{written}` {}: write #[{}({})]",
                        key.because, key.namespace, key.usage.split(", ").next().unwrap_or(key.usage)
                    ),
                ));
            }
            if !key.sites.contains(&site) {
                return Err(syn::Error::new_spanned(
                    meta,
                    format!(
                        "#[{namespace}({written})] applies to {}, not to {}",
                        sites_text(key.sites),
                        site_name(site)
                    ),
                ));
            }
            store(&mut out, key, meta)?;
        }
    }
    Ok(out)
}

/// The item's keys. Also checks the attributes of every field and variant,
/// so each derive reports a misplaced or unknown key anywhere in the type.
pub(crate) fn item(input: &DeriveInput) -> syn::Result<Attrs> {
    let out = parse(&input.attrs, Site::Item)?;
    match &input.data {
        syn::Data::Struct(data) => {
            for f in &data.fields {
                field(f)?;
            }
        }
        syn::Data::Enum(data) => {
            for v in &data.variants {
                variant(v)?;
                for f in &v.fields {
                    field(f)?;
                }
            }
        }
        syn::Data::Union(data) => {
            for f in &data.fields.named {
                field(f)?;
            }
        }
    }
    Ok(out)
}

/// A field's keys.
pub(crate) fn field(field: &syn::Field) -> syn::Result<Attrs> {
    parse(&field.attrs, Site::Field)
}

/// A variant's keys.
pub(crate) fn variant(variant: &syn::Variant) -> syn::Result<Attrs> {
    parse(&variant.attrs, Site::Variant)
}
