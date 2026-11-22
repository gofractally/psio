//! The helper attributes every PSIO derive reads, parsed in one place.
//!
//! `#[psio(...)]` describes the type and is read by every derive. A
//! format-named attribute holds only what is specific to that format:
//!
//! | attribute | key | site | meaning |
//! |-----------|-----|------|---------|
//! | `psio` | `crate = "path"` | item | path of the psio crate (default `::psio`) |
//! | `psio` | `transparent` | item | the type is its one member in every format |
//! | `psio` | `invariant = "path"` | item | `fn(&Self) -> Result<(), E>` with `E: Into<Cow<'static, str>>` (a `&'static str`, or a `String` computed from the value) every reader and writer enforces |
//! | `psio` | `definition_will_not_change` | item | the record never gains members |
//! | `psio` | `variant` | item | a field-less enum is a variant of unit alternatives, not its integer |
//! | `psio` | `custom`, `custom = "id"` | item | the type has a custom text form; the id defaults to the schema name |
//! | `psio` | `name = "..."` | item, variant, field | the name of the type in a schema; the name of an enumerator, alternative or member in a schema and in text |
//! | `psio` | `rename_all = "..."` | item | a case convention for the member names of a struct or the variant names of an enum |
//! | `psio` | `skip` | field | the field is not a member of the type in any format; a reader fills it with its default |
//! | `psio` | `other` | variant | the catch-all of a field-less `#[repr(int)]` enum |
//! | `pssz` | `max_dynamic_data = N` | item | pSSZ cap on the record's variable part |
//! | `json` | `json_number`, `json_string`, `ijson_number`, `ijson_string` | item, field | integer form in Json / IJson text |
//!
//! A key under the wrong attribute, at the wrong site, or unknown is an
//! error. Every derive checks every attribute in this table that it can see,
//! so a misplaced key is reported whichever derives the type uses.
//!
//! # `#[serde(...)]`
//!
//! The derives also read serde's attributes, so a type annotated for serde
//! is described once. An attribute with a PSIO meaning is honoured; one that
//! would make PSIO's text, or what PSIO accepts, differ from what the
//! attribute says is an error on the derives that describe text (`JsonText`,
//! `ToPsszSchema`, `Serde`); one that only steers serde's generated code is
//! ignored. The binary derives ignore an attribute that concerns text only.
//! The rules are in [`serde_key`]; the table for users is in the psio crate
//! docs.

use syn::{Attribute, DeriveInput, Lit, Meta, NestedMeta};

/// The attributes parsed here, in table order.
const NAMESPACES: [&str; 3] = ["psio", "pssz", "json"];

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
    Key { name: "name", namespace: "psio", sites: &[Site::Item, Site::Variant, Site::Field],
          because: "names the type, alternative or member for every format", usage: "name = \"...\"" },
    Key { name: "rename_all", namespace: "psio", sites: &[Site::Item],
          because: "names the members or variants for every format", usage: "rename_all = \"...\"" },
    Key { name: "skip", namespace: "psio", sites: &[Site::Field],
          because: "says what the members of the type are for every format", usage: "skip" },
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
    /// `#[psio(name = "...")]` or `#[serde(rename = "...")]`.
    pub(crate) name: Option<String>,
    /// `#[psio(rename_all = "...")]` or `#[serde(rename_all = "...")]`: on an
    /// item the convention of its member or variant names, on a variant
    /// (serde's spelling only) that of the members of a struct variant.
    pub(crate) rename_all: Option<Case>,
    /// `#[serde(rename_all_fields = "...")]`: the convention of the members
    /// of every struct variant.
    pub(crate) rename_all_fields: Option<Case>,
    /// `#[psio(other)]`.
    pub(crate) other: bool,
    /// `#[pssz(max_dynamic_data = N)]`.
    pub(crate) max_dynamic_data: Option<u64>,
    /// `#[json(json_number)]` and the other three integer forms.
    pub(crate) forms: Forms,
    /// `#[psio(skip)]` or `#[serde(skip)]`.
    pub(crate) skip: bool,
    /// `#[serde(default = "path")]` on a skipped field: what a reader fills
    /// it with instead of `Default::default()`.
    pub(crate) skip_default: Option<syn::Path>,
}

/// An id the Schema IR reserves for PSIO's own types (`psio::pssz::
/// schema_ir::reserved_custom_id`): the ids with a binary meaning, `box`,
/// and the time ids (`time-point`, `duration`, `monotonic`, alone or
/// followed by `-`).
pub(crate) fn reserved_custom_id(id: &str) -> bool {
    matches!(
        id,
        "bool" | "char" | "string" | "utf8" | "hex" | "bitvector" | "bitlist" | "map" | "set" | "unknown" | "box"
    ) || ["time-point", "duration", "monotonic"]
        .iter()
        .any(|kind| id.strip_prefix(kind).is_some_and(|rest| rest.is_empty() || rest.starts_with('-')))
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
                Meta::NameValue(nv) => {
                    let id = string(nv, "custom is a string")?;
                    if reserved_custom_id(&id.value()) {
                        return Err(syn::Error::new_spanned(
                            &id,
                            format!(
                                "\"{}\" is an id PSIO's own types have in a schema (the codecs read a \
                                 meaning into it); a custom type takes an id of its own",
                                id.value()
                            ),
                        ));
                    }
                    Some(id.value())
                }
                _ => return Err(syn::Error::new_spanned(meta, "expected custom or custom = \"id\"")),
            })
        }
        "name" => out.name = Some(string(name_value(meta, key.usage)?, "name is a string")?.value()),
        "rename_all" => {
            let text = string(name_value(meta, key.usage)?, "rename_all is a string")?;
            out.rename_all = Some(Case::parse(&text)?);
        }
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

// ── Case conventions ────────────────────────────────────────────────────────

/// A `rename_all` convention, applied as serde applies it: a field name is
/// taken to be `snake_case` and a variant name `PascalCase`.
#[derive(Clone, Copy, PartialEq)]
pub(crate) enum Case {
    Lower,
    Upper,
    Pascal,
    Camel,
    Snake,
    ScreamingSnake,
    Kebab,
    ScreamingKebab,
}

impl Case {
    fn parse(text: &syn::LitStr) -> syn::Result<Self> {
        Ok(match text.value().as_str() {
            "lowercase" => Self::Lower,
            "UPPERCASE" => Self::Upper,
            "PascalCase" => Self::Pascal,
            "camelCase" => Self::Camel,
            "snake_case" => Self::Snake,
            "SCREAMING_SNAKE_CASE" => Self::ScreamingSnake,
            "kebab-case" => Self::Kebab,
            "SCREAMING-KEBAB-CASE" => Self::ScreamingKebab,
            _ => {
                return Err(syn::Error::new_spanned(
                    text,
                    "rename_all is one of \"lowercase\", \"UPPERCASE\", \"PascalCase\", \
                     \"camelCase\", \"snake_case\", \"SCREAMING_SNAKE_CASE\", \"kebab-case\", \
                     \"SCREAMING-KEBAB-CASE\"",
                ))
            }
        })
    }

    /// The name of a field written `snake_case`.
    pub(crate) fn field(self, name: &str) -> String {
        match self {
            Self::Lower | Self::Snake => name.to_string(),
            Self::Upper | Self::ScreamingSnake => name.to_ascii_uppercase(),
            Self::Pascal => {
                let mut out = String::new();
                let mut capitalize = true;
                for c in name.chars() {
                    if c == '_' {
                        capitalize = true;
                    } else if capitalize {
                        out.push(c.to_ascii_uppercase());
                        capitalize = false;
                    } else {
                        out.push(c);
                    }
                }
                out
            }
            Self::Camel => {
                let pascal = Self::Pascal.field(name);
                lower_first(&pascal)
            }
            Self::Kebab => name.replace('_', "-"),
            Self::ScreamingKebab => name.to_ascii_uppercase().replace('_', "-"),
        }
    }

    /// The name of a variant written `PascalCase`.
    pub(crate) fn variant(self, name: &str) -> String {
        match self {
            Self::Pascal => name.to_string(),
            Self::Lower => name.to_ascii_lowercase(),
            Self::Upper => name.to_ascii_uppercase(),
            Self::Camel => lower_first(name),
            Self::Snake => {
                let mut out = String::new();
                for (i, c) in name.char_indices() {
                    if i > 0 && c.is_uppercase() {
                        out.push('_');
                    }
                    out.push(c.to_ascii_lowercase());
                }
                out
            }
            Self::ScreamingSnake => Self::Snake.variant(name).to_ascii_uppercase(),
            Self::Kebab => Self::Snake.variant(name).replace('_', "-"),
            Self::ScreamingKebab => Self::ScreamingSnake.variant(name).replace('_', "-"),
        }
    }
}

fn lower_first(name: &str) -> String {
    let mut chars = name.chars();
    match chars.next() {
        Some(first) => first.to_ascii_lowercase().to_string() + chars.as_str(),
        None => String::new(),
    }
}

/// The name of a member in text and in a schema: `#[psio(name)]` /
/// `#[serde(rename)]`, else the identifier under the type's convention.
pub(crate) fn member_name(field: &syn::Field, attrs: &Attrs, case: Option<Case>) -> String {
    if let Some(name) = &attrs.name {
        return name.clone();
    }
    let ident = field.ident.as_ref().map(|i| i.to_string()).unwrap_or_default();
    let ident = ident.strip_prefix("r#").unwrap_or(&ident);
    match case {
        Some(case) => case.field(ident),
        None => ident.to_string(),
    }
}

/// The name of an enumerator or alternative in text and in a schema:
/// `#[psio(name)]` / `#[serde(rename)]`, else the identifier under the
/// enum's convention.
pub(crate) fn variant_name(variant: &syn::Variant, attrs: &Attrs, case: Option<Case>) -> String {
    if let Some(name) = &attrs.name {
        return name.clone();
    }
    let ident = variant.ident.to_string();
    match case {
        Some(case) => case.variant(&ident),
        None => ident,
    }
}

// ── The derive that is reading ──────────────────────────────────────────────

/// Which derive is reading the attributes. It decides what a serde attribute
/// means: the derives that describe text reject one PSIO's text cannot
/// honour, the binary derives ignore one that concerns text only.
#[derive(Clone, Copy, PartialEq)]
pub(crate) enum Derive {
    JsonText,
    /// `ToPsszSchema`: the schema describes the text the schema-driven
    /// codecs read and write.
    Schema,
    /// `Serde`: serde impls that read and write PSIO's text.
    Serde,
    /// `PsszPack`, `PsszUnpack`, `PsszView`.
    Pssz,
    Key,
    /// The derives of `psio::experimental::frac`.
    // Constructed only with the feature `experimental-frac`.
    #[allow(dead_code)]
    Frac,
    /// `Capnp*`, `Fb*`, `Wit*`, `ToDynamicSchema`: every field is a member.
    // Constructed only with the features `capnp`, `flatbuf`, `wit`,
    // `dynamic-view`.
    #[allow(dead_code)]
    Interchange(&'static str),
}

impl Derive {
    fn text(self) -> bool {
        matches!(self, Self::JsonText | Self::Schema | Self::Serde)
    }
    fn name(self) -> &'static str {
        match self {
            Self::JsonText => "JsonText",
            Self::Schema => "ToPsszSchema",
            Self::Serde => "Serde",
            Self::Pssz => "PsszPack / PsszUnpack / PsszView",
            Self::Key => "Key",
            Self::Frac => "the frac derives",
            Self::Interchange(name) => name,
        }
    }
}

/// Where an attribute list is being read from.
#[derive(Clone, Copy)]
struct Context {
    site: Site,
    /// The derive, when its rules are to be applied; `None` reads the values
    /// only (the item was already checked).
    derive: Option<Derive>,
    /// The field's type is `Option<...>`.
    optional: bool,
    /// The field belongs to an enum variant.
    in_variant: bool,
}

// ── #[serde(...)] ───────────────────────────────────────────────────────────

/// What `rename = ...` and `rename_all = ...` say: one name for both
/// directions, or different names for writing and reading.
enum Sides {
    Both(syn::LitStr),
    Split,
}

fn sides(meta: &Meta) -> Option<Sides> {
    match meta {
        Meta::NameValue(nv) => match &nv.lit {
            Lit::Str(value) => Some(Sides::Both(value.clone())),
            _ => None,
        },
        Meta::List(list) => {
            let side = |name: &str| {
                list.nested.iter().find_map(|item| match item {
                    NestedMeta::Meta(Meta::NameValue(nv)) if nv.path.is_ident(name) => {
                        match &nv.lit {
                            Lit::Str(value) => Some(value.clone()),
                            _ => None,
                        }
                    }
                    _ => None,
                })
            };
            match (side("serialize"), side("deserialize")) {
                (Some(write), Some(read)) if write.value() == read.value() => {
                    Some(Sides::Both(write))
                }
                _ => Some(Sides::Split),
            }
        }
        Meta::Path(_) => None,
    }
}

fn rejected(meta: &Meta, derive: Derive, why: &str) -> syn::Error {
    let key = path_text(meta.path());
    syn::Error::new_spanned(
        meta,
        format!("#[serde({key})] cannot be combined with derive({}): {why}", derive.name()),
    )
}

/// What the serde attributes of one site said that is settled only once the
/// whole site was read.
#[derive(Default)]
struct Serde {
    name: Option<(String, Meta)>,
    skip: Option<Meta>,
    skip_serializing: Option<Meta>,
    skip_deserializing: Option<Meta>,
    default: Option<Meta>,
    conversions: Vec<Meta>,
}

/// One key of a `#[serde(...)]` attribute.
///
/// | key | site | derives that describe text | binary derives |
/// |-----|------|----------------------------|----------------|
/// | `transparent` | item | `#[psio(transparent)]` | the same |
/// | `rename = "n"` | item, variant, field | `#[psio(name = "n")]` | ignored (a schema name on the item) |
/// | `rename(serialize, deserialize)` with two names | any | error: text has one name | ignored |
/// | `rename_all = "c"` | item | `#[psio(rename_all = "c")]` | ignored |
/// | `rename_all = "c"` | variant | the members of a struct variant | ignored |
/// | `rename_all_fields = "c"` | item | the members of every struct variant | ignored |
/// | `skip` | field | `#[psio(skip)]` | the same |
/// | `skip_serializing` + `skip_deserializing` | field | `#[psio(skip)]` | the same |
/// | `skip_serializing` or `skip_deserializing` alone | field | error | error |
/// | `skip` | variant | error | error |
/// | `skip_serializing`, `skip_deserializing` | variant | error | ignored |
/// | `skip_serializing_if = "Option::is_none"` | field | accepted: PSIO writes `null` | ignored |
/// | `skip_serializing_if`, another predicate | field | error | ignored |
/// | `default` | field of type `Option` | accepted: PSIO's rule | ignored |
/// | `default`, `default = "path"` | another field, item | error | ignored |
/// | `default = "path"` | skipped field | what a reader fills the field with | the same |
/// | `flatten` | field | error | ignored |
/// | `tag`, `content`, `untagged` | item, variant | error | ignored |
/// | `deny_unknown_fields` | item | error | ignored |
/// | `alias` | variant, field | error | ignored |
/// | `with`, `serialize_with`, `deserialize_with` | variant, field | error | ignored |
/// | `from`, `try_from`, `into` | item | error unless `#[psio(custom)]` (`ToPsszSchema`) | ignored |
/// | `other` | variant | error | ignored |
/// | `variant_identifier`, `field_identifier` | item | error | ignored |
/// | `bound`, `borrow`, `crate`, `expecting`, `remote`, `getter` | any | ignored | ignored |
fn serde_key(out: &mut Attrs, seen: &mut Serde, meta: &Meta, ctx: Context) -> syn::Result<()> {
    let key = path_text(meta.path());
    let text = ctx.derive.filter(|derive| derive.text());
    let reject = |why: &str| match text {
        Some(derive) => Err(rejected(meta, derive, why)),
        None => Ok(()),
    };
    match (key.as_str(), ctx.site) {
        ("transparent", Site::Item) => out.transparent = true,
        ("rename", _) => match sides(meta) {
            Some(Sides::Both(name)) => seen.name = Some((name.value(), meta.clone())),
            Some(Sides::Split) => reject(
                "PSIO text has one name for writing and reading; give both sides the same name",
            )?,
            None => {}
        },
        ("rename_all", Site::Item | Site::Variant) | ("rename_all_fields", Site::Item) => {
            match sides(meta) {
                Some(Sides::Both(case)) => {
                    let case = Case::parse(&case)?;
                    if key == "rename_all" {
                        out.rename_all = Some(case);
                    } else {
                        out.rename_all_fields = Some(case);
                    }
                }
                Some(Sides::Split) => reject(
                    "PSIO text has one name for writing and reading; give both sides the same \
                     convention",
                )?,
                None => {}
            }
        }
        ("skip", Site::Field) => seen.skip = Some(meta.clone()),
        ("skip_serializing", Site::Field) => seen.skip_serializing = Some(meta.clone()),
        ("skip_deserializing", Site::Field) => seen.skip_deserializing = Some(meta.clone()),
        ("skip", Site::Variant) => {
            if let Some(derive) = ctx.derive {
                return Err(rejected(
                    meta,
                    derive,
                    "every variant is an alternative of the type with its own index; PSIO has \
                     no skipped alternatives",
                ));
            }
        }
        ("skip_serializing" | "skip_deserializing", Site::Variant) => reject(
            "PSIO writes and reads every alternative of a type; there is no one-directional \
             variant",
        )?,
        ("skip_serializing_if", Site::Field) => {
            let none = match meta {
                Meta::NameValue(syn::MetaNameValue { lit: Lit::Str(path), .. }) => {
                    let path: String = path.value().split_whitespace().collect();
                    let path = path.trim_start_matches("::");
                    matches!(
                        path,
                        "Option::is_none" | "core::option::Option::is_none"
                            | "std::option::Option::is_none"
                    )
                }
                _ => false,
            };
            if !none {
                reject(
                    "PSIO writes every member; the only predicate accepted is \
                     \"Option::is_none\", for which PSIO writes null and reads a missing member \
                     as None",
                )?
            }
        }
        ("default", Site::Field) => seen.default = Some(meta.clone()),
        ("default", Site::Item) => reject(
            "in PSIO text a missing member is an error unless it is an Option, which is None; \
             C++ and Rust accept the same input",
        )?,
        ("flatten", _) => reject(
            "PSIO text and the Schema IR have no flattened members: a member is one key of its \
             record",
        )?,
        ("tag" | "content" | "untagged", _) => reject(
            "PSIO has one text for a variant: {\"Name\": value} under IJson (serde's default, \
             externally tagged) and [index, value] under Json",
        )?,
        ("deny_unknown_fields", _) => reject(
            "PSIO readers skip unknown members, which is what lets a record gain members",
        )?,
        ("alias", _) => reject("PSIO text has one name for a member or variant")?,
        ("with" | "serialize_with" | "deserialize_with", _) => reject(
            "PSIO text is defined by the member's type; give the type a text form of its own \
             with psio::json_text_form! and #[psio(custom)]",
        )?,
        ("from" | "try_from" | "into", Site::Item) => seen.conversions.push(meta.clone()),
        ("other", Site::Variant) => reject(
            "serde's catch-all drops the tag it read; PSIO's is #[psio(other)] Unknown(<repr>), \
             which keeps the number",
        )?,
        ("variant_identifier" | "field_identifier", _) => reject(
            "PSIO reads an enumerator from its name, its number, the numeric string or \
             \"name:number\"",
        )?,
        // bound, borrow, crate, expecting, remote, getter and anything serde
        // adds later: serde's own business.
        _ => {}
    }
    Ok(())
}

/// The path of `#[serde(default = "path")]`.
fn default_path(meta: &Meta) -> Option<syn::Path> {
    match meta {
        Meta::NameValue(syn::MetaNameValue { lit: Lit::Str(path), .. }) => path.parse().ok(),
        _ => None,
    }
}

/// Settles what needed the whole site.
fn serde_site(out: &mut Attrs, psio_name: Option<&Meta>, seen: Serde, ctx: Context) -> syn::Result<()> {
    let text = ctx.derive.filter(|derive| derive.text());
    if let Some((name, meta)) = seen.name {
        match (&out.name, psio_name) {
            (Some(own), Some(_)) if *own != name => {
                return Err(syn::Error::new_spanned(
                    meta,
                    format!("#[serde(rename = \"{name}\")] and #[psio(name = \"{own}\")] disagree"),
                ))
            }
            _ => out.name = Some(name),
        }
    }
    // A member is either part of the type or not.
    match (&seen.skip, &seen.skip_serializing, &seen.skip_deserializing) {
        (Some(_), _, _) | (None, Some(_), Some(_)) => out.skip = true,
        (None, Some(one), None) | (None, None, Some(one)) => {
            if let Some(derive) = ctx.derive {
                return Err(rejected(
                    one,
                    derive,
                    "a field is a member of the type in both directions or in neither; use \
                     #[psio(skip)] (or #[serde(skip)]) to leave it out",
                ));
            }
        }
        (None, None, None) => {}
    }
    if let Some(meta) = &seen.default {
        let path = default_path(meta);
        if out.skip {
            out.skip_default = path;
        } else if let Some(derive) = text {
            if path.is_some() || !matches!(meta, Meta::Path(_)) {
                return Err(rejected(
                    meta,
                    derive,
                    "in PSIO text a missing member is an error unless it is an Option, which is \
                     None; a member cannot have another default",
                ));
            }
            if !ctx.optional {
                return Err(rejected(
                    meta,
                    derive,
                    "in PSIO text a missing member is an error unless it is an Option, which is \
                     None; make the member an Option or remove the attribute",
                ));
            }
        }
    }
    for meta in &seen.conversions {
        match text {
            Some(Derive::Schema) if out.custom.is_some() => {}
            Some(derive) => {
                return Err(rejected(
                    meta,
                    derive,
                    "the text of the type would be that of the conversion type; give the type \
                     a text form of its own with psio::json_text_form! and #[psio(custom)]",
                ))
            }
            None => {}
        }
    }
    Ok(())
}

// ── Reading one site ────────────────────────────────────────────────────────

fn is_option(ty: &syn::Type) -> bool {
    match ty {
        syn::Type::Path(path) if path.qself.is_none() => {
            let text = path_text(&path.path);
            matches!(text.as_str(), "Option" | "core::option::Option" | "std::option::Option")
        }
        _ => false,
    }
}

/// The table's keys on one item, field or variant.
fn parse(attrs: &[Attribute], ctx: Context) -> syn::Result<Attrs> {
    let site = ctx.site;
    let mut out = Attrs::default();
    let mut seen = Serde::default();
    let mut psio_name = None;
    let mut psio_skip = None;
    for attr in attrs {
        if attr.path.is_ident("serde") {
            // A malformed serde attribute is serde's to report.
            if let Ok(Meta::List(list)) = attr.parse_meta() {
                for item in &list.nested {
                    if let NestedMeta::Meta(meta) = item {
                        serde_key(&mut out, &mut seen, meta, ctx)?;
                    }
                }
            }
            continue;
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
            match key.name {
                "name" => psio_name = Some(meta.clone()),
                "skip" => psio_skip = Some(meta.clone()),
                _ => {}
            }
        }
    }
    let serde_skip = seen.skip.clone().or(seen.skip_serializing.clone());
    serde_site(&mut out, psio_name.as_ref(), seen, ctx)?;
    if let (true, Some(derive)) = (out.skip, ctx.derive) {
        let at = psio_skip.or(serde_skip);
        let fail = |why: String| match &at {
            Some(meta) => Err(syn::Error::new_spanned(meta, why)),
            None => Err(syn::Error::new(proc_macro2::Span::call_site(), why)),
        };
        if ctx.in_variant {
            return fail(
                "a skipped field is a field of a struct: an enum alternative is its payload"
                    .to_string(),
            );
        }
        if let Derive::Interchange(name) = derive {
            return fail(format!(
                "derive({name}) has no skipped fields: every field is a member of the type"
            ));
        }
    }
    Ok(out)
}

/// The item's keys, read for `derive`. Also checks the attributes of every
/// field and variant, so each derive reports a misplaced or unknown key, or
/// a serde attribute it cannot honour, anywhere in the type.
pub(crate) fn item(input: &DeriveInput, derive: Derive) -> syn::Result<Attrs> {
    let derive = Some(derive);
    let out = parse(
        &input.attrs,
        Context { site: Site::Item, derive, optional: false, in_variant: false },
    )?;
    let check_field = |f: &syn::Field, in_variant: bool| {
        parse(
            &f.attrs,
            Context { site: Site::Field, derive, optional: is_option(&f.ty), in_variant },
        )
    };
    match &input.data {
        syn::Data::Struct(data) => {
            for f in &data.fields {
                check_field(f, false)?;
            }
        }
        syn::Data::Enum(data) => {
            for v in &data.variants {
                parse(
                    &v.attrs,
                    Context { site: Site::Variant, derive, optional: false, in_variant: false },
                )?;
                for f in &v.fields {
                    check_field(f, true)?;
                }
            }
        }
        syn::Data::Union(data) => {
            for f in &data.fields.named {
                check_field(f, false)?;
            }
        }
    }
    Ok(out)
}

/// A field's keys. The derive's rules were applied by [`item`].
pub(crate) fn field(field: &syn::Field) -> syn::Result<Attrs> {
    parse(
        &field.attrs,
        Context { site: Site::Field, derive: None, optional: is_option(&field.ty), in_variant: false },
    )
}

/// A variant's keys. The derive's rules were applied by [`item`].
pub(crate) fn variant(variant: &syn::Variant) -> syn::Result<Attrs> {
    parse(
        &variant.attrs,
        Context { site: Site::Variant, derive: None, optional: false, in_variant: false },
    )
}

// ── Members ─────────────────────────────────────────────────────────────────

/// A field that is a member of the type (not skipped).
pub(crate) struct MemberField<'a> {
    pub(crate) member: syn::Member,
    pub(crate) field: &'a syn::Field,
    pub(crate) attrs: Attrs,
}

fn member_of(index: usize, field: &syn::Field) -> syn::Member {
    match &field.ident {
        Some(ident) => syn::Member::Named(ident.clone()),
        None => syn::Member::Unnamed(index.into()),
    }
}

/// The members of a struct or of a variant's payload: its fields without
/// the skipped ones, in declaration order.
pub(crate) fn members(fields: &syn::Fields) -> syn::Result<Vec<MemberField<'_>>> {
    let mut out = Vec::new();
    for (index, f) in fields.iter().enumerate() {
        let attrs = field(f)?;
        if !attrs.skip {
            out.push(MemberField { member: member_of(index, f), field: f, attrs });
        }
    }
    Ok(out)
}

/// `path { .. }` / `path(..)` from one value per member, in member order. A
/// skipped field is `Default::default()`, or the result of its
/// `#[serde(default = "path")]`.
pub(crate) fn construct(
    path: proc_macro2::TokenStream,
    fields: &syn::Fields,
    values: &[proc_macro2::TokenStream],
) -> syn::Result<proc_macro2::TokenStream> {
    let mut values = values.iter();
    let mut initializers = Vec::new();
    for (index, f) in fields.iter().enumerate() {
        let attrs = field(f)?;
        let value = if attrs.skip {
            match &attrs.skip_default {
                Some(path) => quote::quote!(#path()),
                None => quote::quote!(::core::default::Default::default()),
            }
        } else {
            let value = values.next().expect("one value per member");
            quote::quote!(#value)
        };
        let member = member_of(index, f);
        initializers.push(match &member {
            syn::Member::Named(_) => quote::quote!(#member: #value),
            syn::Member::Unnamed(_) => value,
        });
    }
    Ok(match fields {
        syn::Fields::Named(_) => quote::quote!(#path { #(#initializers),* }),
        syn::Fields::Unnamed(_) => quote::quote!(#path ( #(#initializers),* )),
        syn::Fields::Unit => path,
    })
}
