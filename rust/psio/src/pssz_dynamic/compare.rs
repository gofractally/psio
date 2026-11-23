//! Comparing two types of two Schema IRs at run time (C++
//! `psio/pssz_ir_compare.hpp`): are they the same type ([`equivalent`]), and
//! may one replace the other ([`evolves_from`])?
//!
//! ```
//! use psio::pssz::dynamic::{evolves_from, Compare};
//! use psio::pssz::schema_ir::{schema_for, AnyType};
//! use psio::{PsszPack, PsszUnpack, ToPsszSchema};
//!
//! #[derive(PsszPack, PsszUnpack, ToPsszSchema)]
//! #[pssz(max_dynamic_data = 200)]
//! struct RowV1 { id: u32, name: String }
//! #[derive(PsszPack, PsszUnpack, ToPsszSchema)]
//! #[pssz(max_dynamic_data = 200)]
//! struct RowV2 { id: u32, name: String, note: Option<String> }
//!
//! let (installed, old) = schema_for::<RowV1>().unwrap();
//! let (proposed, new) = schema_for::<RowV2>().unwrap();
//! assert!(evolves_from(&installed, &old, &proposed, &new, Compare::Contract).ok);
//! let back = evolves_from(&proposed, &new, &installed, &old, Compare::Contract);
//! assert_eq!((back.code, back.path.as_str()), ("fields-removed", "(root)"));
//! // A type is also named: `&AnyType::named("Row")`.
//! # let _ = AnyType::named("Row");
//! ```
//!
//! `evolves_from(old, Old, new, New)` is the run-time twin of the
//! compile-time [`pssz_evolves_from`](crate::pssz::pssz_evolves_from) and
//! applies the rules of docs/pssz-spec.md 7.5 / 7.6 / 16 to two types that
//! are only known as schemas: New's reader reads every value Old's writer
//! can produce, with the same bytes. `equivalent` is the symmetric relation:
//! the same values, the same bytes, the same permitted revisions.
//!
//! Both are taken in the automatic width profile. Named aliases are
//! followed and their names are not compared; recursive types are compared
//! co-inductively.
//!
//! [`Compare::Wire`] compares the bytes and which bytes are values: the kind
//! of every node, length bounds (wherever written: in the type or on the
//! member), every offset width, `definitionWillNotChange`, `maxDynamicData`,
//! `maxFields`, whether a value is boxed, and the key type of a map / set
//! (equivalent in both relations). [`Compare::Contract`] (the default) adds
//! what a consumer of the text forms and of the type's identity sees: member
//! and alternative names, custom ids, enums, the text shape of a record,
//! `jsonInt` / `ijsonInt`, and whether a variant is `untagged`. (An alternative's
//! label is its name: one renamed is `alternative-renamed`.)
//!
//! The result is a [`Compatibility`]: `ok`, or the first rule broken as a
//! stable `code` (shared with every language through
//! `conformance/evolution-vectors.json`), a sentence, and a `path` from the
//! root: `.field`, `[]` (an element; a map entry's key and value are `[].0`
//! / `[].1`), `.2` (a tuple element), `<Alt>`, `?` (an optional's payload),
//! `(nested)`, for functions `name(args)` / `name(result)`, for an
//! interface's listed type `type:Name`, for whole schemas the interface name
//! and `/`. The root itself is `(root)`.
//!
//! Functions ([`evolves_from_func`]): the argument list ([`argument_list`])
//! and the result both evolve Old -> New; the names of the two functions are
//! not compared. Interfaces ([`evolves_from_interface`]): functions and
//! listed types are matched by name. Schemas ([`evolves_from_schema`]):
//! interfaces are matched by name.
use super::{
    assoc_shape, bit_shape, can_be_empty, dwnc, find_attribute, layout_of, metadata, parse_cap,
    parse_length_bound, parse_max_fields, Codec, LengthBound,
};
use crate::pssz::{Pssz, PsszError, PsszResult};
use crate::pssz_schema_ir::{
    wire_custom_id, AnyType, AnyValue, Attribute, Custom, Enum, Func, Interface, Member, Object,
    Schema,
};

/// What [`equivalent`] / [`evolves_from`] compare.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Compare {
    /// The bytes, and which bytes are values.
    Wire,
    /// `Wire`, and names / custom ids / enums / text forms.
    #[default]
    Contract,
}

/// The outcome of a comparison: compatible, or the first rule broken.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Compatibility {
    pub ok: bool,
    /// The stable identifier of the rule; empty when `ok`.
    pub code: &'static str,
    /// A sentence for a person; not part of any contract.
    pub reason: String,
    /// Where, from the root; empty when `ok`.
    pub path: String,
}
impl Compatibility {
    fn compatible() -> Self {
        Self { ok: true, code: "", reason: String::new(), path: String::new() }
    }
    /// `Ok(())`, or the comparison itself as the error.
    pub fn into_result(self) -> Result<(), Compatibility> {
        if self.ok {
            Ok(())
        } else {
            Err(self)
        }
    }
}
impl std::fmt::Display for Compatibility {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.ok {
            f.write_str("compatible")
        } else {
            write!(f, "{}: {} [{}]", self.path, self.reason, self.code)
        }
    }
}
impl std::error::Error for Compatibility {}

/// The argument list of a function as a type (C++
/// `psio::pssz_ir::argument_list`): the `Object` of its parameters with the
/// attribute `tuple` = `"named"` or `"positional"` (members named `"0"`,
/// `"1"`, … when positional) and the function's `maxFields` /
/// `maxDynamicData`.
pub fn argument_list(f: &Func) -> AnyType {
    // Positional names are "0", "1", ...: no parameter is named so.
    let named = f
        .params
        .iter()
        .enumerate()
        .all(|(i, param)| !param.name.is_empty() && param.name != i.to_string());
    let members = f
        .params
        .iter()
        .enumerate()
        .map(|(i, param)| Member {
            name: if named { param.name.clone() } else { i.to_string() },
            r#type: param.r#type.clone(),
            attributes: param.attributes.clone(),
        })
        .collect();
    let mut attributes = vec![Attribute {
        name: "tuple".into(),
        value: Some(if named { "named" } else { "positional" }.into()),
    }];
    for name in ["maxFields", "maxDynamicData"] {
        if let Some(found) = find_attribute(&f.attributes, name) {
            attributes.push(found.clone());
        }
    }
    AnyType::new(AnyValue::Object(Object { members, attributes }))
}

/// The comparison stopped: the rule is in `Comparer::out`.
struct Stop;
type Walk<T = ()> = Result<T, Stop>;

#[derive(Clone, Copy, Default, PartialEq, Eq)]
struct Pos {
    /// The cap of the nearest enclosing record.
    cap: Option<u64>,
    /// Of the member holding the position.
    bound: Option<LengthBound>,
}
impl Pos {
    fn beneath(&self) -> Pos {
        Pos { cap: self.cap, bound: None }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Kind {
    Integer,
    Floating,
    Boolean,
    String,
    Bytes,
    Bitvector,
    Bitlist,
    Map,
    Set,
    Option,
    List,
    Array,
    Variant,
    Record,
    Nested,
    Unsupported,
}
impl Kind {
    fn name(self) -> &'static str {
        match self {
            Kind::Integer => "integer",
            Kind::Floating => "float",
            Kind::Boolean => "bool",
            Kind::String => "string",
            Kind::Bytes => "bytes",
            Kind::Bitvector => "bit vector",
            Kind::Bitlist => "bit list",
            Kind::Map => "map",
            Kind::Set => "set",
            Kind::Option => "optional",
            Kind::List => "list",
            Kind::Array => "array",
            Kind::Variant => "variant",
            Kind::Record => "record",
            Kind::Nested => "nested value",
            Kind::Unsupported => "unsupported type",
        }
    }
    fn of(ty: &AnyType) -> Kind {
        match &ty.value {
            AnyValue::Int(_) => Kind::Integer,
            AnyValue::Float(_) => Kind::Floating,
            AnyValue::Option(_) => Kind::Option,
            AnyValue::List(_) | AnyValue::BoundedList(_) => Kind::List,
            AnyValue::Array(_) => Kind::Array,
            AnyValue::Variant(_) => Kind::Variant,
            AnyValue::Tuple(_) | AnyValue::Object(_) | AnyValue::Struct(_) => Kind::Record,
            AnyValue::Nested(_) => Kind::Nested,
            AnyValue::Custom(c) => match c.id.as_str() {
                "bool" => Kind::Boolean,
                "char" => Kind::Integer,
                "string" | "utf8" => Kind::String,
                "hex" => Kind::Bytes,
                "bitvector" => Kind::Bitvector,
                "bitlist" => Kind::Bitlist,
                "map" => Kind::Map,
                "set" => Kind::Set,
                _ => Kind::Unsupported,
            },
            _ => Kind::Unsupported,
        }
    }
}

/// A type with the nodes that add nothing to its bytes taken off.
struct Peeled<'t> {
    wire: &'t AnyType,
    boxed: bool,
    ids: Vec<&'t str>,
    enumeration: Option<&'t Enum>,
    kind: Kind,
}

/// The lengths a position admits.
#[derive(Clone, Copy, PartialEq, Eq)]
struct Range {
    min: u64,
    max: Option<u64>,
}
fn number(n: Option<u64>) -> String {
    n.map_or_else(|| "unbounded".to_string(), |n| n.to_string())
}
impl Range {
    /// The bound of the container type (a List has none, a BoundedList a
    /// maximum, an Array one length) and of the member that holds it.
    fn of(container: Option<&AnyType>, bound: Option<&LengthBound>) -> Range {
        let mut r = Range { min: 0, max: None };
        match container.map(|c| &c.value) {
            Some(AnyValue::BoundedList(list)) => r.max = Some(list.maxCount),
            Some(AnyValue::Array(array)) => {
                r.max = Some(array.len);
                r.min = array.len;
            }
            _ => (),
        }
        if let Some(bound) = bound {
            let lower = bound.exact.or(bound.min);
            let upper = bound.exact.or(bound.max);
            if let Some(lower) = lower.filter(|lower| *lower > r.min) {
                r.min = lower;
            }
            if let Some(upper) = upper.filter(|upper| r.max.is_none_or(|max| *upper < max)) {
                r.max = Some(upper);
            }
        }
        r
    }
    fn text(&self) -> String {
        format!("[{}, {}]", self.min, number(self.max))
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Text {
    Object,
    NamedArguments,
    Positional,
}
struct RecordShape<'t> {
    types: Vec<&'t AnyType>,
    /// Empty for a Tuple.
    members: Vec<&'t Member>,
    attributes: Option<&'t [Attribute]>,
    stable: bool,
    /// A Tuple node: no cap scope of its own.
    tuple: bool,
    text: Text,
}
fn attribute_value<'t>(values: &'t [Attribute], name: &str) -> Option<&'t str> {
    find_attribute(values, name)?.value.as_deref()
}
impl<'t> RecordShape<'t> {
    fn of(ty: &'t AnyType) -> Self {
        let named = |members: &'t [Member], attributes: &'t [Attribute], stable, text| RecordShape {
            types: members.iter().map(|m| m.r#type.as_ref()).collect(),
            members: members.iter().collect(),
            attributes: Some(attributes),
            stable,
            tuple: false,
            text,
        };
        match &ty.value {
            AnyValue::Struct(s) => named(&s.members, &s.attributes, true, Text::Object),
            AnyValue::Object(o) => named(
                &o.members,
                &o.attributes,
                dwnc(&o.attributes),
                match attribute_value(&o.attributes, "tuple") {
                    None => Text::Object,
                    Some("named") => Text::NamedArguments,
                    Some(_) => Text::Positional,
                },
            ),
            AnyValue::Tuple(t) => RecordShape {
                types: t.members.iter().map(|m| m.as_ref()).collect(),
                members: Vec::new(),
                attributes: None,
                stable: false,
                tuple: true,
                text: Text::Positional,
            },
            _ => unreachable!("a record kind"),
        }
    }
    fn member_step(&self, i: usize) -> String {
        if !self.members.is_empty() && self.text != Text::Positional {
            format!(".{}", self.members[i].name)
        } else {
            format!(".{i}")
        }
    }
}

fn custom(ty: &AnyType) -> Option<&Custom> {
    match &ty.value {
        AnyValue::Custom(custom) => Some(custom),
        _ => None,
    }
}
fn element(ty: &AnyType) -> &AnyType {
    match &ty.value {
        AnyValue::List(list) => &list.r#type,
        AnyValue::BoundedList(list) => &list.r#type,
        AnyValue::Array(array) => &array.r#type,
        _ => unreachable!("a list kind"),
    }
}

struct Comparer<'a> {
    schema_a: &'a Schema,
    schema_b: &'a Schema,
    codec_a: Codec<'a>,
    codec_b: Codec<'a>,
    names: bool,
    out: Compatibility,
    path: String,
    /// Synthesized argument lists: alive (and at one address) for the walk,
    /// which keys what it has seen and the widths it derived by address.
    made: Vec<Box<AnyType>>,
    nesting: usize,
    seen: Vec<(usize, usize, Pos, Pos, bool)>,
}

impl<'a> Comparer<'a> {
    fn run(
        a: &'a Schema,
        b: &'a Schema,
        what: Compare,
        body: impl FnOnce(&mut Comparer<'a>) -> Walk,
    ) -> Compatibility {
        let malformed = |first: bool, error: PsszError| Compatibility {
            ok: false,
            code: "malformed-schema",
            reason: format!("the {} schema: {}", if first { "first" } else { "second" }, error.0),
            path: "(root)".into(),
        };
        let codec_a = match Codec::new::<Pssz>(a) {
            Ok(codec) => codec,
            Err(error) => return malformed(true, error),
        };
        let codec_b = match Codec::new::<Pssz>(b) {
            Ok(codec) => codec,
            Err(error) => return malformed(false, error),
        };
        let mut comparer = Comparer {
            schema_a: a,
            schema_b: b,
            codec_a,
            codec_b,
            names: what == Compare::Contract,
            out: Compatibility::compatible(),
            path: String::new(),
            made: Vec::new(),
            nesting: 0,
            seen: Vec::new(),
        };
        let _ = body(&mut comparer);
        comparer.out
    }

    fn fail<T>(&mut self, code: &'static str, reason: impl Into<String>) -> Walk<T> {
        if self.out.ok {
            self.out = Compatibility {
                ok: false,
                code,
                reason: reason.into(),
                path: if self.path.is_empty() { "(root)".into() } else { self.path.clone() },
            };
        }
        Err(Stop)
    }
    fn good<T>(&mut self, first: bool, result: PsszResult<T>) -> Walk<T> {
        match result {
            Ok(value) => Ok(value),
            Err(error) => self.fail(
                "malformed-schema",
                format!("the {} schema: {}", if first { "first" } else { "second" }, error.0),
            ),
        }
    }
    fn schema(&self, first: bool) -> &'a Schema {
        if first {
            self.schema_a
        } else {
            self.schema_b
        }
    }
    /// Runs `body` one step further down the path.
    fn at<T>(&mut self, step: &str, body: impl FnOnce(&mut Self) -> Walk<T>) -> Walk<T> {
        let size = self.path.len();
        self.path.push_str(step);
        let result = body(self);
        self.path.truncate(size);
        result
    }

    fn peel<'t>(&mut self, first: bool, input: &'t AnyType) -> Walk<Peeled<'t>>
    where
        'a: 't,
    {
        let schema = self.schema(first);
        let mut p = Peeled { wire: input, boxed: false, ids: Vec::new(), enumeration: None, kind: Kind::Unsupported };
        let mut at = input;
        for _ in 0..=4 * schema.types.len() + 64 {
            match &at.value {
                AnyValue::Enum(named) => {
                    self.good(first, schema.enum_underlying(&named.r#type).map(|_| ()))?;
                    p.enumeration.get_or_insert(named);
                    at = &named.r#type;
                }
                AnyValue::Custom(custom) if !wire_custom_id(&custom.id) => {
                    if custom.id == "box" {
                        p.boxed = true;
                    } else {
                        p.ids.push(&custom.id);
                    }
                    at = &custom.r#type;
                }
                AnyValue::Type(_) => at = self.good(first, schema.resolve(at))?,
                _ => {
                    p.wire = at;
                    p.kind = Kind::of(at);
                    return Ok(p);
                }
            }
        }
        self.fail("malformed-schema", "cyclic named aliases")
    }

    fn ranges(&mut self, was: Range, is: Range, exact: bool, what: &str) -> Walk {
        if exact {
            if was != is {
                return self.fail(
                    "bound-changed",
                    format!("the length bound of {what} differs ({} / {})", was.text(), is.text()),
                );
            }
            return Ok(());
        }
        if is.min > was.min || is.max.is_some_and(|max| was.max.is_none_or(|old| max < old)) {
            return self.fail(
                "bound-tightened",
                format!("the length bound of {what} was tightened ({} -> {})", was.text(), is.text()),
            );
        }
        Ok(())
    }
    /// The offset width the codec derives for `ty` (resolved) at `pos`.
    fn width(&mut self, first: bool, ty: &AnyType, pos: &Pos) -> Walk<usize> {
        let codec = if first { &self.codec_a } else { &self.codec_b };
        let count = pos.bound.as_ref().and_then(LengthBound::count);
        let width = codec.beneath(pos.cap, || codec.width_of(ty, count));
        self.good(first, width)
    }
    fn widths(&mut self, a: &AnyType, b: &AnyType, pa: &Pos, pb: &Pos, what: &str) -> Walk {
        let was = self.width(true, a, pa)?;
        let is = self.width(false, b, pb)?;
        if was != is {
            return self.fail("width-changed", format!("the offset width of {what} differs ({was} / {is})"));
        }
        Ok(())
    }

    fn enums(&mut self, a: &Peeled<'_>, b: &Peeled<'_>, exact: bool) -> Walk {
        let (was, is) = match (a.enumeration, b.enumeration) {
            (None, None) => return Ok(()),
            (Some(was), Some(is)) => (&was.members, &is.members),
            _ => return self.fail("enum-changed", "an enum on one side only"),
        };
        let has = |all: &[crate::pssz_schema_ir::Enumerator], e: &crate::pssz_schema_ir::Enumerator| {
            all.iter().find(|other| other.name == e.name).is_some_and(|other| other.value == e.value)
        };
        for e in was {
            if !has(is, e) {
                return self
                    .fail("enum-changed", format!("the enumerator \"{}\" was removed or renumbered", e.name));
            }
        }
        if exact {
            for e in is {
                if !has(was, e) {
                    return self
                        .fail("enum-changed", format!("the enumerator \"{}\" is on one side only", e.name));
                }
            }
        }
        Ok(())
    }

    fn record(&mut self, a: &Peeled<'_>, b: &Peeled<'_>, pa: &Pos, pb: &Pos, exact: bool) -> Walk {
        let sa = RecordShape::of(a.wire);
        let sb = RecordShape::of(b.wire);
        if sa.stable != sb.stable {
            return self.fail("dwnc-changed", "definitionWillNotChange on one side only");
        }
        if self.names && sa.text != sb.text {
            return self.fail(
                "text-form-changed",
                "the text shape of the record differs (an object, a named argument list or positional)",
            );
        }
        let count = sa.types.len();
        if exact && sb.types.len() != count {
            return self.fail(
                "field-count-changed",
                format!("the number of fields differs ({count} / {})", sb.types.len()),
            );
        }
        if sb.types.len() < count {
            return self
                .fail("fields-removed", format!("fields were removed ({count} -> {})", sb.types.len()));
        }
        if sa.stable && sb.types.len() != count {
            return self.fail("dwnc-gained-fields", "a definitionWillNotChange record gained fields");
        }
        // A record's width is its own wherever it appears; a tuple's is also
        // bounded by the cap around it.
        if !sa.stable {
            self.widths(a.wire, b.wire, pa, pb, "the record")?;
        }
        // The members lie beneath the record's own cap, or beneath none; a
        // Tuple node has none of its own and passes on the one around it.
        let (mut below_a, mut below_b) = (pa.beneath(), pb.beneath());
        let (mut cap_a, mut cap_b, mut fields_a, mut fields_b) = (None, None, None, None);
        if let Some(attributes) = sa.attributes {
            cap_a = self.good(true, parse_cap(attributes))?;
            fields_a = self.good(true, parse_max_fields(attributes))?;
        }
        if let Some(attributes) = sb.attributes {
            cap_b = self.good(false, parse_cap(attributes))?;
            fields_b = self.good(false, parse_max_fields(attributes))?;
        }
        if !sa.tuple {
            below_a.cap = cap_a;
        }
        if !sb.tuple {
            below_b.cap = cap_b;
        }
        for i in 0..count {
            let size = self.path.len();
            let step = sa.member_step(i);
            self.at(&step, |this| {
                let (mut ma, mut mb) = (below_a, below_b);
                if let Some(member) = sa.members.get(i) {
                    ma.bound = this.good(true, parse_length_bound(&member.attributes))?;
                }
                if let Some(member) = sb.members.get(i) {
                    mb.bound = this.good(false, parse_length_bound(&member.attributes))?;
                }
                if let (true, Some(member_a), Some(member_b)) = (this.names, sa.members.get(i), sb.members.get(i)) {
                    if sa.text != Text::Positional && member_a.name != member_b.name {
                        // (reported at the record: the member has two names)
                        this.path.truncate(size);
                        return this.fail(
                            "field-renamed",
                            format!(
                                "field {i} was renamed or reordered (\"{}\" -> \"{}\")",
                                member_a.name, member_b.name
                            ),
                        );
                    }
                    for form in ["jsonInt", "ijsonInt"] {
                        if attribute_value(&member_a.attributes, form) != attribute_value(&member_b.attributes, form) {
                            return this.fail(
                                "text-form-changed",
                                format!("{form} differs (an integer as a number or a string)"),
                            );
                        }
                    }
                }
                this.walk(sa.types[i], sb.types[i], &ma, &mb, exact)
            })?;
        }
        // An appended member must be able to be absent.
        for i in count..sb.types.len() {
            let info = self.good(false, layout_of(self.schema_b, sb.types[i]))?;
            if !info.optional {
                let which = if sb.members.is_empty() || sb.text == Text::Positional {
                    i.to_string()
                } else {
                    format!("\"{}\"", sb.members[i].name)
                };
                return self.fail(
                    "appended-field-not-optional",
                    format!("the appended field {which} is not optional (old data has no value for it)"),
                );
            }
        }
        // Caps (docs/pssz-spec.md 7.5, 16).
        if exact {
            if cap_a != cap_b {
                return self.fail(
                    "max-dynamic-data",
                    format!("maxDynamicData differs ({} / {}; unbounded: none)", number(cap_a), number(cap_b)),
                );
            }
            if fields_a != fields_b {
                return self.fail(
                    "max-fields",
                    format!("maxFields differs ({} / {}; unbounded: none)", number(fields_a), number(fields_b)),
                );
            }
            return Ok(());
        }
        if let Some(cap_b) = cap_b {
            let limit = match cap_a {
                Some(cap) => cap,
                None => {
                    let bound = self.codec_a.bound_of(a.wire, None, false, &mut Vec::new());
                    match self.good(true, bound)? {
                        Some(bound) => bound as u64,
                        None => {
                            return self.fail(
                                "max-dynamic-data",
                                format!("maxDynamicData ({cap_b}) was added to a record of unbounded size"),
                            )
                        }
                    }
                }
            };
            if limit > cap_b {
                return self.fail(
                    "max-dynamic-data",
                    match cap_a {
                        Some(cap_a) => format!("maxDynamicData was lowered ({cap_a} -> {cap_b})"),
                        None => format!(
                            "maxDynamicData ({cap_b}) is below the bound inferred for the old record ({limit})"
                        ),
                    },
                );
            }
        }
        if let Some(fields_a) = fields_a {
            if sb.types.len() as u64 > fields_a {
                return self.fail(
                    "max-fields",
                    format!("more fields ({}) than the original's maxFields ({fields_a})", sb.types.len()),
                );
            }
            match fields_b {
                None => return self.fail("max-fields", format!("maxFields ({fields_a}) was dropped")),
                Some(fields_b) if fields_b > fields_a => {
                    return self.fail("max-fields", format!("maxFields was raised ({fields_a} -> {fields_b})"))
                }
                _ => (),
            }
        }
        Ok(())
    }

    fn walk(&mut self, ta: &AnyType, tb: &AnyType, pa: &Pos, pb: &Pos, exact: bool) -> Walk {
        let a = self.peel(true, ta)?;
        let b = self.peel(false, tb)?;
        if a.boxed != b.boxed {
            return self.fail("boxed-changed", "a boxed value on one side only");
        }
        if self.names {
            if a.ids != b.ids {
                return self.fail(
                    "custom-id-changed",
                    format!(
                        "a custom type on one side only, or under another id (\"{}\" / \"{}\")",
                        a.ids.first().copied().unwrap_or(""),
                        b.ids.first().copied().unwrap_or("")
                    ),
                );
            }
            self.enums(&a, &b, exact)?;
        }
        // (a recursive type: the pair is being, or has been, compared)
        let key = (a.wire as *const AnyType as usize, b.wire as *const AnyType as usize, *pa, *pb, exact);
        if self.seen.contains(&key) {
            return Ok(());
        }
        self.seen.push(key);
        if a.kind != b.kind {
            return self
                .fail("type-changed", format!("the type differs ({} / {})", a.kind.name(), b.kind.name()));
        }
        if a.kind == Kind::Unsupported {
            return self.fail("malformed-schema", "a type the codec does not support");
        }
        if self.nesting >= 512 {
            return self.fail("malformed-schema", "type nesting too deep");
        }
        self.nesting += 1;
        let result = self.by_kind(&a, &b, pa, pb, exact);
        self.nesting -= 1;
        result
    }

    fn by_kind(&mut self, a: &Peeled<'_>, b: &Peeled<'_>, pa: &Pos, pb: &Pos, exact: bool) -> Walk {
        let la = self.good(true, layout_of(self.schema_a, a.wire))?;
        let lb = self.good(false, layout_of(self.schema_b, b.wire))?;
        let (below_a, below_b) = (pa.beneath(), pb.beneath());
        let fixed_size = |this: &mut Self| -> Walk {
            if la.fixed != lb.fixed || (la.fixed && la.size != lb.size) {
                return this.fail(
                    "fixed-size-changed",
                    "a fixed-size type on one side and a variable-size type on the other, or another fixed size",
                );
            }
            Ok(())
        };
        match a.kind {
            Kind::Integer => {
                let integer = |ty: &AnyType| match &ty.value {
                    AnyValue::Int(i) => (i.bits, i.isSigned),
                    _ => (8, false), // "char"
                };
                if integer(a.wire) != integer(b.wire) {
                    return self.fail("integer-changed", "the integer type differs (bits or signedness)");
                }
                Ok(())
            }
            Kind::Floating => {
                if la.size != lb.size {
                    return self.fail("float-changed", "the float type differs");
                }
                Ok(())
            }
            Kind::Boolean => Ok(()),
            Kind::Bitvector | Kind::Bitlist => {
                let (Some(ca), Some(cb)) = (custom(a.wire), custom(b.wire)) else { unreachable!() };
                let (na, _) = self.good(true, bit_shape(self.schema_a, ca))?;
                let (nb, _) = self.good(false, bit_shape(self.schema_b, cb))?;
                if a.kind == Kind::Bitvector {
                    if na != nb {
                        return self
                            .fail("bit-count-changed", format!("the bit count differs ({na} / {nb})"));
                    }
                    return Ok(());
                }
                self.ranges(
                    Range { min: 0, max: Some(na as u64) },
                    Range { min: 0, max: Some(nb as u64) },
                    exact,
                    "the bit list",
                )
            }
            Kind::String | Kind::Bytes => {
                let (Some(ca), Some(cb)) = (custom(a.wire), custom(b.wire)) else { unreachable!() };
                if ca.id != cb.id {
                    return self.fail(
                        "utf8-changed",
                        "utf8 on one side only (a raw string and a UTF-8 string are different types)",
                    );
                }
                fixed_size(self)?;
                let container_a = self.good(true, self.schema_a.direct(&ca.r#type))?;
                let container_b = self.good(false, self.schema_b.direct(&cb.r#type))?;
                self.ranges(
                    Range::of(Some(container_a), pa.bound.as_ref()),
                    Range::of(Some(container_b), pb.bound.as_ref()),
                    exact,
                    if a.kind == Kind::String { "the string" } else { "the bytes" },
                )
            }
            Kind::Nested => {
                self.ranges(
                    Range::of(None, pa.bound.as_ref()),
                    Range::of(None, pb.bound.as_ref()),
                    exact,
                    "the nested value",
                )?;
                let (AnyValue::Nested(na), AnyValue::Nested(nb)) = (&a.wire.value, &b.wire.value) else {
                    unreachable!()
                };
                // The inner value is a root of its own: no cap or bound reaches it.
                self.at("(nested)", |this| this.walk(&na.r#type, &nb.r#type, &Pos::default(), &Pos::default(), exact))
            }
            Kind::Option => {
                let (AnyValue::Option(oa), AnyValue::Option(ob)) = (&a.wire.value, &b.wire.value) else {
                    unreachable!()
                };
                let empty_a = self.good(true, can_be_empty(self.schema_a, &oa.r#type, 0))?;
                let empty_b = self.good(false, can_be_empty(self.schema_b, &ob.r#type, 0))?;
                if empty_a != empty_b {
                    return self.fail(
                        "optional-form-changed",
                        "the optional's presence form differs (a selector byte or not)",
                    );
                }
                // The member's bound and the cap reach the payload.
                self.at("?", |this| this.walk(&oa.r#type, &ob.r#type, pa, pb, exact))
            }
            Kind::List | Kind::Array => {
                if let (AnyValue::Array(array_a), AnyValue::Array(array_b)) = (&a.wire.value, &b.wire.value) {
                    if array_a.len != array_b.len {
                        return self.fail(
                            "array-length-changed",
                            format!("the array length differs ({} / {})", array_a.len, array_b.len),
                        );
                    }
                } else {
                    self.ranges(
                        Range::of(Some(a.wire), pa.bound.as_ref()),
                        Range::of(Some(b.wire), pb.bound.as_ref()),
                        exact,
                        "the list",
                    )?;
                }
                let (ea, eb) = (element(a.wire), element(b.wire));
                let elem_a = self.good(true, layout_of(self.schema_a, ea))?;
                let elem_b = self.good(false, layout_of(self.schema_b, eb))?;
                if !(elem_a.fixed && elem_b.fixed) {
                    self.widths(
                        a.wire,
                        b.wire,
                        pa,
                        pb,
                        if a.kind == Kind::Array { "the array" } else { "the list" },
                    )?;
                }
                self.at("[]", |this| this.walk(ea, eb, &below_a, &below_b, exact))
            }
            Kind::Map | Kind::Set => {
                let (Some(ca), Some(cb)) = (custom(a.wire), custom(b.wire)) else { unreachable!() };
                let sa = self.good(true, assoc_shape(self.schema_a, ca))?;
                let sb = self.good(false, assoc_shape(self.schema_b, cb))?;
                let map = a.kind == Kind::Map;
                let what = if map { "the map" } else { "the set" };
                self.ranges(
                    Range::of(Some(sa.list), pa.bound.as_ref()),
                    Range::of(Some(sb.list), pb.bound.as_ref()),
                    exact,
                    what,
                )?;
                let elem_a = self.good(true, layout_of(self.schema_a, sa.element))?;
                let elem_b = self.good(false, layout_of(self.schema_b, sb.element))?;
                if !(elem_a.fixed && elem_b.fixed) {
                    self.widths(sa.list, sb.list, pa, pb, what)?;
                }
                // Another key type is another order (docs/pssz-spec.md
                // 5.9.4): the keys are the same type, in both relations.
                let keys = self.at(if map { "[].0" } else { "[]" }, |this| {
                    this.walk(sa.key, sb.key, &below_a, &below_b, true)
                });
                if keys.is_err() {
                    self.out.code = "key-type-changed";
                    self.out.reason = format!("the key type of {what} differs: {}", self.out.reason);
                    return Err(Stop);
                }
                let (Some(value_a), Some(value_b)) = (sa.value, sb.value) else { return Ok(()) };
                // The entry is a tuple (key, value): its width, then the value.
                let entry_a = self.good(true, self.schema_a.resolve(sa.element))?;
                let entry_b = self.good(false, self.schema_b.resolve(sb.element))?;
                self.at("[]", |this| this.widths(entry_a, entry_b, &below_a, &below_b, "the map entry"))?;
                self.at("[].1", |this| this.walk(value_a, value_b, &below_a, &below_b, exact))
            }
            Kind::Variant => {
                let (AnyValue::Variant(va), AnyValue::Variant(vb)) = (&a.wire.value, &b.wire.value) else {
                    unreachable!()
                };
                if exact && va.members.len() != vb.members.len() {
                    return self.fail(
                        "alternative-count-changed",
                        format!(
                            "the number of alternatives differs ({} / {})",
                            va.members.len(),
                            vb.members.len()
                        ),
                    );
                }
                if vb.members.len() < va.members.len() {
                    return self.fail(
                        "alternatives-removed",
                        format!(
                            "variant alternatives were removed ({} -> {})",
                            va.members.len(),
                            vb.members.len()
                        ),
                    );
                }
                if self.names
                    && find_attribute(&va.attributes, "untagged").is_some()
                        != find_attribute(&vb.attributes, "untagged").is_some()
                {
                    return self.fail("text-form-changed", "an untagged variant on one side only");
                }
                for (i, (ma, mb)) in va.members.iter().zip(&vb.members).enumerate() {
                    if self.names && ma.name != mb.name {
                        return self.fail(
                            "alternative-renamed",
                            format!(
                                "alternative {i} was renamed or reordered (\"{}\" -> \"{}\")",
                                ma.name, mb.name
                            ),
                        );
                    }
                    self.at(&format!("<{}>", ma.name), |this| {
                        this.walk(&ma.r#type, &mb.r#type, &below_a, &below_b, exact)
                    })?;
                }
                Ok(())
            }
            Kind::Record => {
                fixed_size(self)?;
                self.record(a, b, pa, pb, exact)
            }
            Kind::Unsupported => self.fail("malformed-schema", "a type the codec does not support"),
        }
    }

    /// Both types are well-formed (and of a version this library reads).
    fn check(&mut self, a: &AnyType, b: &AnyType) -> Walk {
        self.good(true, metadata(self.schema_a, a, true))?;
        self.good(false, metadata(self.schema_b, b, true))
    }
    fn types(&mut self, a: &AnyType, b: &AnyType, exact: bool) -> Walk {
        self.check(a, b)?;
        self.walk(a, b, &Pos::default(), &Pos::default(), exact)
    }

    fn function(&mut self, fa: &Func, fb: &Func, exact: bool) -> Walk {
        let args_a = Box::new(argument_list(fa));
        let args_b = Box::new(argument_list(fb));
        let arguments = self.at("(args)", |this| this.types(&args_a, &args_b, exact));
        self.made.push(args_a);
        self.made.push(args_b);
        arguments?;
        self.at("(result)", |this| {
            let (result_a, result_b) = match (&fa.result, &fb.result) {
                (None, None) => return Ok(()),
                (Some(a), Some(b)) => (a, b),
                _ => return this.fail("result-changed", "a result on one side only"),
            };
            let bound = |f: &Func| {
                let own: Vec<Attribute> = find_attribute(&f.attributes, "result.lengthBound")
                    .map(|found| Attribute { name: "lengthBound".into(), value: found.value.clone() })
                    .into_iter()
                    .collect();
                parse_length_bound(&own)
            };
            let pa = Pos { cap: None, bound: this.good(true, bound(fa))? };
            let pb = Pos { cap: None, bound: this.good(false, bound(fb))? };
            this.check(result_a, result_b)?;
            this.walk(result_a, result_b, &pa, &pb, exact)
        })
    }

    fn interface_pair(&mut self, ia: &Interface, ib: &Interface, exact: bool) -> Walk {
        let find = |interface: &'_ Interface, name: &str| -> Option<usize> {
            interface.funcs.iter().position(|f| f.name == name)
        };
        for f in &ia.funcs {
            self.at(&f.name, |this| match find(ib, &f.name) {
                None => this.fail("function-removed", "the function was removed"),
                Some(other) => this.function(f, &ib.funcs[other], exact),
            })?;
        }
        if exact {
            for f in &ib.funcs {
                if find(ia, &f.name).is_none() {
                    return self
                        .at(&f.name, |this| this.fail("function-added", "the function is on one side only"));
                }
            }
        }
        for name in &ia.type_names {
            self.at(&format!("type:{name}"), |this| {
                if !ib.type_names.contains(name) {
                    return this.fail("type-removed", "the type was removed from the interface");
                }
                let reference = AnyType::named(name.clone());
                this.types(&reference, &reference, exact)
            })?;
        }
        if exact {
            for name in &ib.type_names {
                if !ia.type_names.contains(name) {
                    return self.at(&format!("type:{name}"), |this| {
                        this.fail("type-added", "the type is on one side only")
                    });
                }
            }
        }
        Ok(())
    }

    fn schemas(&mut self, exact: bool) -> Walk {
        let (a, b) = (self.schema_a, self.schema_b);
        for interface in &a.interfaces {
            self.at(&format!("{}/", interface.name), |this| {
                match b.interfaces.iter().find(|other| other.name == interface.name) {
                    None => this.fail("interface-removed", "the interface was removed"),
                    Some(other) => this.interface_pair(interface, other, exact),
                }
            })?;
        }
        if exact {
            for interface in &b.interfaces {
                if !a.interfaces.iter().any(|other| other.name == interface.name) {
                    return self.at(&format!("{}/", interface.name), |this| {
                        this.fail("interface-added", "the interface is on one side only")
                    });
                }
            }
        }
        Ok(())
    }
}

// ── Two types ───────────────────────────────────────────────────────────────
//
// `type_a` / `type_b` are types of their schemas: an AnyType taken from one
// (`schema.get("Row")`, a member's type), or a name: `&AnyType::named("Row")`.

/// Whether the two are the same type: the same values, the same bytes, the
/// same permitted revisions (and, under [`Compare::Contract`], the same
/// text). C++ `psio::pssz_ir::equivalent`.
pub fn equivalent(
    schema_a: &Schema,
    type_a: &AnyType,
    schema_b: &Schema,
    type_b: &AnyType,
    what: Compare,
) -> Compatibility {
    Comparer::run(schema_a, schema_b, what, |c| c.types(type_a, type_b, true))
}
/// Whether `new_type` may replace `old_type`: its reader reads every value
/// the old writer can produce (docs/pssz-spec.md 7.5 / 7.6). C++
/// `psio::pssz_ir::evolves_from`.
pub fn evolves_from(
    old_schema: &Schema,
    old_type: &AnyType,
    new_schema: &Schema,
    new_type: &AnyType,
    what: Compare,
) -> Compatibility {
    Comparer::run(old_schema, new_schema, what, |c| c.types(old_type, new_type, false))
}

// ── Two functions: argument list and result ─────────────────────────────────

/// [`equivalent`] for two functions: their argument lists and results.
pub fn equivalent_func(
    schema_a: &Schema,
    func_a: &Func,
    schema_b: &Schema,
    func_b: &Func,
    what: Compare,
) -> Compatibility {
    Comparer::run(schema_a, schema_b, what, |c| c.function(func_a, func_b, true))
}
/// [`evolves_from`] for two functions: a new callee accepts every call an
/// old caller makes, and a stored result of the old function is a value of
/// the new result type.
pub fn evolves_from_func(
    old_schema: &Schema,
    old_func: &Func,
    new_schema: &Schema,
    new_func: &Func,
    what: Compare,
) -> Compatibility {
    Comparer::run(old_schema, new_schema, what, |c| c.function(old_func, new_func, false))
}

// ── Two interfaces: every function and every listed type, by name ───────────

/// [`equivalent`] for two interfaces: the same functions and listed types,
/// by name, each equivalent.
pub fn equivalent_interface(
    schema_a: &Schema,
    interface_a: &Interface,
    schema_b: &Schema,
    interface_b: &Interface,
    what: Compare,
) -> Compatibility {
    Comparer::run(schema_a, schema_b, what, |c| c.interface_pair(interface_a, interface_b, true))
}
/// [`evolves_from`] for two interfaces: every function and listed type of
/// the old one exists in the new one, by name, and evolves.
pub fn evolves_from_interface(
    old_schema: &Schema,
    old_interface: &Interface,
    new_schema: &Schema,
    new_interface: &Interface,
    what: Compare,
) -> Compatibility {
    Comparer::run(old_schema, new_schema, what, |c| c.interface_pair(old_interface, new_interface, false))
}

// ── Two whole schemas: every interface, by name ─────────────────────────────

/// [`equivalent`] for two schemas: the same interfaces, by name, each
/// equivalent.
pub fn equivalent_schema(schema_a: &Schema, schema_b: &Schema, what: Compare) -> Compatibility {
    Comparer::run(schema_a, schema_b, what, |c| c.schemas(true))
}
/// [`evolves_from`] for two schemas: every interface of the old one exists
/// in the new one, by name, and evolves.
pub fn evolves_from_schema(old_schema: &Schema, new_schema: &Schema, what: Compare) -> Compatibility {
    Comparer::run(old_schema, new_schema, what, |c| c.schemas(false))
}
