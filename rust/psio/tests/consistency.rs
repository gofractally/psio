//! The consistency round (the Rust mirror of `cpp/tests/consistency_tests.cpp`):
//! rules every implementation has to agree on that no shared fixture row
//! pins yet — what `pssz_evolves_from` allows, `sorted` / `unique` /
//! `unique_keys`, `max_fields`, the schema limits, key writers and strict
//! validation.

use psio::json::{self, IJson, Json};
use psio::key::{from_key, key_bytes, try_to_key, validate_key};
use psio::pssz::dynamic;
use psio::pssz::schema_ir::{self, AnyType, AnyValue as V, Attribute, Member, Schema};
use psio::pssz::upgrade::validate_optional_upgrade;
use psio::pssz::{
    auto_pssz_width, decode_strict, pssz_evolves_from, validate_strict, BoundedList, BoundedRawString,
    BoundedString, Pssz, PsszPack, RawString,
};
use std::collections::{BTreeMap, BTreeSet};

// ── pssz_evolves_from: member annotations and caps (item 7) ─────────────────

macro_rules! evo {
    ($name:ident, $cap:tt, [$($owner:tt)*] $owner_ty:ty, [$($tags:tt)*] $tags_ty:ty) => {
        #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack)]
        #[cfg_attr(any(), doc = "")]
        #[pssz(max_dynamic_data = $cap)]
        struct $name {
            id: u32,
            $($owner)*
            owner: $owner_ty,
            $($tags)*
            tags: $tags_ty,
        }
    };
    ($name:ident, [$($owner:tt)*] $owner_ty:ty, [$($tags:tt)*] $tags_ty:ty) => {
        #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack)]
        struct $name {
            id: u32,
            $($owner)*
            owner: $owner_ty,
            $($tags)*
            tags: $tags_ty,
        }
    };
}
type Raws = Vec<RawString>;
evo!(V1, 1024, [#[psio(max = 32)]] String, [#[psio(max = 8, element(max = 16))]] Raws);
evo!(Same, 1024, [#[psio(max = 32)]] String, [#[psio(max = 8, element(max = 16))]] Raws);
evo!(Wider, 1024, [#[psio(max = 64)]] String, [#[psio(max = 8, element(max = 16))]] Raws);
evo!(Tighter, 1024, [#[psio(max = 16)]] String, [#[psio(max = 8, element(max = 16))]] Raws);
evo!(Raw, 1024, [#[psio(max = 32)]] RawString, [#[psio(max = 8, element(max = 16))]] Raws);
evo!(ElemWider, 1024, [#[psio(max = 32)]] String, [#[psio(max = 8, element(max = 20))]] Raws);
evo!(ElemTighter, 1024, [#[psio(max = 32)]] String, [#[psio(max = 8, element(max = 8))]] Raws);
evo!(ElemUtf8, 1024, [#[psio(max = 32)]] String, [#[psio(max = 8, element(max = 16))]] Vec<String>);
evo!(CountWider, 1024, [#[psio(max = 32)]] String, [#[psio(max = 12, element(max = 16))]] Raws);
evo!(Unbounded, 1024, [] String, [#[psio(max = 8, element(max = 16))]] Raws);
evo!(Sorted, 1024, [#[psio(max = 32)]] String, [#[psio(max = 8, sorted, element(max = 16))]] Raws);
evo!(CapWider, 4096, [#[psio(max = 32)]] String, [#[psio(max = 8, element(max = 16))]] Raws);
evo!(CapTighter, 512, [#[psio(max = 32)]] String, [#[psio(max = 8, element(max = 16))]] Raws);
evo!(CapOtherWidth, 100000, [#[psio(max = 32)]] String, [#[psio(max = 8, element(max = 16))]] Raws);
// The same bound spelled with wrapper types: one type in Rust.
evo!(Wrapped, 1024, [] BoundedString<32>, [] BoundedList<BoundedRawString<16>, 8>);
// Without a cap every type has the width of its own bound: the record is 2
// bytes wide and its list of tags 1.
evo!(Open, [#[psio(max = 300)]] String, [#[psio(max = 8, element(max = 16))]] Raws);
evo!(OpenWideList, [#[psio(max = 300)]] String, [#[psio(max = 8, element(max = 40))]] Raws);
evo!(OpenWider, [#[psio(max = 400)]] String, [#[psio(max = 8, element(max = 16))]] Raws);
evo!(OpenCapped, 600, [#[psio(max = 300)]] String, [#[psio(max = 8, element(max = 16))]] Raws);
evo!(OpenCappedLow, 400, [#[psio(max = 300)]] String, [#[psio(max = 8, element(max = 16))]] Raws);

macro_rules! fields {
    ($name:ident $(, max_fields = $max:literal)? { $($field:ident),* }) => {
        #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema)]
        $(#[psio(max_fields = $max)])?
        #[pssz(max_dynamic_data = 64)]
        struct $name { a: u32, $($field: Option<u32>),* }
    };
}
fields!(Fields3, max_fields = 3 { b });
fields!(Fields3Grown, max_fields = 3 { b, c });
// Within its own cap, beyond the original's.
fields!(Fields3Raised, max_fields = 8 { b, c, d });
fields!(Fields3Dropped { b, c });
// Lowered to the current count: the promise still holds.
fields!(Fields3Lowered, max_fields = 2 { b });

const _: () = {
    assert!(pssz_evolves_from::<V1, Same>());
    // A bound may only widen (the new reader must read every old record),
    // and only while every width stays the same.
    assert!(pssz_evolves_from::<V1, Wider>());
    assert!(!pssz_evolves_from::<V1, Tighter>());
    assert!(!pssz_evolves_from::<Wider, V1>());
    assert!(pssz_evolves_from::<V1, ElemWider>());
    assert!(!pssz_evolves_from::<V1, ElemTighter>());
    assert!(pssz_evolves_from::<V1, CountWider>());
    assert!(!pssz_evolves_from::<CountWider, V1>());
    // Under the cap the widths are the cap's.
    assert!(pssz_evolves_from::<V1, Unbounded>());
    assert!(!pssz_evolves_from::<Unbounded, V1>());
    // utf8 / raw and sorted are the member's type.
    assert!(!pssz_evolves_from::<V1, Raw>());
    assert!(!pssz_evolves_from::<Raw, V1>());
    assert!(!pssz_evolves_from::<V1, ElemUtf8>());
    assert!(!pssz_evolves_from::<V1, Sorted>());
    assert!(!pssz_evolves_from::<Sorted, V1>());
    assert!(pssz_evolves_from::<Sorted, Sorted>());
    // An annotation and a wrapper type are one type.
    assert!(pssz_evolves_from::<V1, Wrapped>());
    assert!(pssz_evolves_from::<Wrapped, V1>());
    assert!(pssz_evolves_from::<Wrapped, Wider>());
    // The cap may grow within its width class and may not shrink.
    assert!(pssz_evolves_from::<V1, CapWider>());
    assert!(!pssz_evolves_from::<V1, CapTighter>());
    assert!(!pssz_evolves_from::<V1, CapOtherWidth>());
    // Adding a cap: only one the old type's inferred bound fits.
    let inferred = match <Open as PsszPack<Pssz>>::INFERRED_BOUND {
        Some(bound) => bound,
        None => panic!("Open has an inferred bound"),
    };
    assert!(inferred <= 600 && inferred > 400);
    assert!(pssz_evolves_from::<Open, OpenCapped>());
    assert!(!pssz_evolves_from::<Open, OpenCappedLow>());
    // Dropping it: nothing old is refused.
    assert!(pssz_evolves_from::<OpenCapped, Open>());
    // Without a cap, a widened bound that moves a nested list's offset
    // table into another width class changes the bytes.
    assert!(auto_pssz_width::<Open>() == auto_pssz_width::<OpenWideList>());
    assert!(!pssz_evolves_from::<Open, OpenWideList>());
    assert!(pssz_evolves_from::<Open, OpenWider>());

    // max_fields: a revision has no more fields than the original's cap and
    // does not raise or drop it.
    assert!(pssz_evolves_from::<Fields3, Fields3Grown>());
    assert!(!pssz_evolves_from::<Fields3, Fields3Raised>());
    assert!(!pssz_evolves_from::<Fields3, Fields3Dropped>());
    assert!(pssz_evolves_from::<Fields3, Fields3Lowered>());
    assert!(!pssz_evolves_from::<Fields3Lowered, Fields3Grown>());
    // Declaring one later is allowed.
    assert!(pssz_evolves_from::<Fields3Dropped, Fields3Grown>());
};

#[test]
fn what_the_evolution_check_promises_holds_on_bytes() {
    // Every record of V1 is a record of Wider, and a V1 record at its bound
    // is not a record of Tighter.
    let full = V1 { id: 1, owner: "a".repeat(32), tags: vec![RawString(vec![b'b'; 16])] };
    let frame = psio::to_pssz(&full);
    assert!(psio::validate_pssz::<Wider>(&frame).is_ok());
    assert!(psio::validate_pssz::<CapWider>(&frame).is_ok());
    assert!(psio::validate_pssz::<Wrapped>(&frame).is_ok());
    assert!(psio::validate_pssz::<Tighter>(&frame).is_err());
    assert!(psio::validate_pssz::<ElemTighter>(&frame).is_err());
}

#[test]
fn the_schema_upgrade_check_applies_the_max_fields_rule() {
    fn admits<A: schema_ir::ToPsszSchema, B: schema_ir::ToPsszSchema>() -> bool {
        let (a, at) = schema_ir::schema_for::<A>().unwrap();
        let (b, bt) = schema_ir::schema_for::<B>().unwrap();
        validate_optional_upgrade(&a, &at, &b, &bt).is_ok()
    }
    assert!(admits::<Fields3, Fields3Grown>());
    assert!(!admits::<Fields3, Fields3Raised>());
    assert!(!admits::<Fields3, Fields3Dropped>());
    assert!(admits::<Fields3, Fields3Lowered>());
    assert!(!admits::<Fields3Lowered, Fields3Grown>());
    assert!(admits::<Fields3Dropped, Fields3Grown>());
}

// ── sorted / unique / unique_keys (item 12) ─────────────────────────────────

macro_rules! ranked {
    ($name:ident { $($(#[$attr:meta])* $field:ident: $ty:ty,)* }) => {
        #[derive(
            Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::JsonText, psio::Key,
            psio::ToPsszSchema,
        )]
        struct $name { $($(#[$attr])* $field: $ty,)* }
    };
}
ranked!(Ranked {
    #[psio(sorted, max = 8)]
    scores: Vec<u32>,
    #[psio(unique)]
    names: Vec<String>,
    #[psio(unique_keys)]
    entries: Vec<(String, u32)>,
    #[psio(element(sorted))]
    rows: Vec<Vec<u8>>,
    #[psio(sorted)]
    triple: [i16; 3],
    #[psio(unique_keys)]
    ids: Option<Vec<u16>>,
    #[psio(sorted_unique, element(max = 4))]
    codes: Vec<String>,
});
// The same record with no constraint: what an unchecked writer produces.
ranked!(Loose {
    #[psio(max = 8)]
    scores: Vec<u32>,
    names: Vec<String>,
    entries: Vec<(String, u32)>,
    rows: Vec<Vec<u8>>,
    triple: [i16; 3],
    ids: Option<Vec<u16>>,
    #[psio(element(max = 4))]
    codes: Vec<String>,
});

fn good() -> Ranked {
    Ranked {
        scores: vec![1, 1, 2, 300],
        names: vec!["a".into(), "ab".into(), "b".into()],
        entries: vec![("x".into(), 1), ("a".into(), 1)],
        rows: vec![vec![1, 2, 2], vec![], vec![9]],
        triple: [-5, 0, 0],
        ids: Some(vec![3, 1, 2]),
        codes: vec!["a".into(), "b".into()],
    }
}
fn loose(r: &Ranked) -> Loose {
    Loose {
        scores: r.scores.clone(),
        names: r.names.clone(),
        entries: r.entries.clone(),
        rows: r.rows.clone(),
        triple: r.triple,
        ids: r.ids.clone(),
        codes: r.codes.clone(),
    }
}

#[test]
fn order_constraints_are_enforced_by_writers_and_checking_readers() {
    let ok = good();
    let frame = psio::to_pssz(&ok);
    let text = json::try_encode::<Json, _>(&ok).unwrap();
    let key = try_to_key(&ok).unwrap();
    assert!(psio::validate_pssz::<Ranked>(&frame).is_ok());
    assert!(validate_strict::<Pssz, Ranked>(&frame).is_ok());
    assert_eq!(psio::from_pssz::<Ranked>(&frame).unwrap(), ok);
    assert_eq!(psio::from_pssz_trusted::<Ranked>(&frame), ok);
    assert_eq!(json::decode::<Json, Ranked>(&text).unwrap(), ok);
    assert!(validate_key::<Ranked>(&key).is_ok());
    assert_eq!(from_key::<Ranked>(&key).unwrap(), ok);
    // The constraints change no byte, no width, no bound and no schema.
    assert_eq!(frame, psio::to_pssz(&loose(&ok)));
    assert_eq!(key, try_to_key(&loose(&ok)).unwrap());
    assert_eq!(
        <Ranked as PsszPack<Pssz>>::INFERRED_BOUND,
        <Loose as PsszPack<Pssz>>::INFERRED_BOUND
    );
    assert_eq!(
        <Ranked as PsszPack<Pssz>>::MAX_ENCODED_SIZE,
        <Loose as PsszPack<Pssz>>::MAX_ENCODED_SIZE
    );
    assert_eq!(auto_pssz_width::<Ranked>(), auto_pssz_width::<Loose>());
    let (ranked, ranked_root) = schema_ir::schema_for::<Ranked>().unwrap();
    let (plain, plain_root) = schema_ir::schema_for::<Loose>().unwrap();
    assert_eq!(ranked.get("Ranked"), plain.get("Loose"));
    assert_eq!((ranked.types.len(), &ranked_root), (plain.types.len(), &AnyType::named("Ranked")));
    assert_eq!(plain_root, AnyType::named("Loose"));

    let refused = |what: &str, change: &dyn Fn(&mut Ranked), message: &str| {
        let mut bad = good();
        change(&mut bad);
        // Writers.
        assert_eq!(psio::try_to_pssz(&bad).unwrap_err().message(), message, "{what}");
        assert!(json::try_encode::<Json, _>(&bad).is_err(), "{what}");
        assert!(json::try_encode::<IJson, _>(&bad).is_err(), "{what}");
        assert!(try_to_key(&bad).is_err(), "{what}");
        // Readers, on what an unchecked writer produces.
        let raw = loose(&bad);
        let frame = psio::to_pssz(&raw);
        assert_eq!(psio::validate_pssz::<Ranked>(&frame).unwrap_err().message(), message, "{what}");
        assert!(validate_strict::<Pssz, Ranked>(&frame).is_err(), "{what}");
        assert_eq!(psio::from_pssz::<Ranked>(&frame).unwrap_err().message(), message, "{what}");
        assert!(decode_strict::<Pssz, Ranked>(&frame).is_err(), "{what}");
        let text = json::try_encode::<Json, _>(&raw).unwrap();
        assert!(json::decode::<Json, Ranked>(&text).is_err(), "{what}");
        assert!(json::validate::<Json, Ranked>(&text).is_err(), "{what}");
        let text = json::try_encode::<IJson, _>(&raw).unwrap();
        assert!(json::decode::<IJson, Ranked>(&text).is_err(), "{what}");
        let key = key_bytes(&raw);
        assert!(validate_key::<Ranked>(&key).is_err(), "{what}");
        assert!(from_key::<Ranked>(&key).is_err(), "{what}");
    };
    const ORDER: &str = "sorted: elements are not in order";
    const STRICT: &str = "sorted: elements are not in strictly ascending order";
    const KEYS: &str = "unique_keys: two elements have the same key";
    refused("scores descend", &|r| r.scores = vec![2, 1], ORDER);
    refused("names repeat", &|r| r.names = vec!["a".into(), "a".into()], STRICT);
    refused("names descend", &|r| r.names = vec!["b".into(), "a".into()], STRICT);
    refused("entries repeat a key", &|r| r.entries = vec![("a".into(), 1), ("b".into(), 2), ("a".into(), 3)], KEYS);
    refused("a row descends", &|r| r.rows = vec![vec![1], vec![3, 2]], ORDER);
    refused("the array descends", &|r| r.triple = [1, 0, 2], ORDER);
    refused("ids repeat", &|r| r.ids = Some(vec![7, 8, 7]), KEYS);
    refused("codes repeat", &|r| r.codes = vec!["a".into(), "a".into()], STRICT);
    // The order is that of the key encodings, as for maps and sets: for
    // integers, strings and tuples of them the order of the values.
    let mut signed = good();
    signed.triple = [-1, 0, 1];
    assert!(psio::try_to_pssz(&signed).is_ok());
    signed.triple = [1, -1, 0];
    assert!(psio::try_to_pssz(&signed).is_err());
    // The bound beside the constraint is still the bound.
    let mut long = good();
    long.scores = (0..9).collect();
    assert_eq!(psio::try_to_pssz(&long).unwrap_err().message(), "pssz: member length_bound violated");
    let mut wide = good();
    wide.codes = vec!["abcde".into()];
    assert!(psio::try_to_pssz(&wide).is_err());
}

// ── max_fields (items 12, 17) ───────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema, psio::JsonText)]
#[psio(max_fields = 4)]
struct Capped {
    a: u32,
    b: Option<String>,
}

#[psio::interface]
pub trait Service {
    #[psio(max_fields = 4)]
    #[pssz(max_dynamic_data = 64)]
    fn put(&mut self, id: u32, #[psio(max = 8)] text: RawString);
    fn plain(&mut self, id: u32);
}

fn attribute(schema: &Schema, ty: &str, name: &str) -> Option<String> {
    match &schema.resolve(schema.get(ty)?).ok()?.value {
        V::Object(object) => object.attributes.iter().find(|a| a.name == name)?.value.clone(),
        _ => None,
    }
}

#[test]
fn max_fields_is_the_cap_on_the_number_of_fields() {
    // In the Schema IR it is the Object's attribute, beside maxDynamicData.
    let mut builder = schema_ir::SchemaBuilder::new();
    builder.insert::<Capped>().unwrap();
    builder.insert_named::<service::args::put>("Service.put").unwrap();
    builder.insert_interface::<service::Tag>().unwrap();
    let schema = builder.finish();
    assert_eq!(attribute(&schema, "Capped", "maxFields").as_deref(), Some("4"));
    assert_eq!(attribute(&schema, "Service.put", "maxFields").as_deref(), Some("4"));
    assert_eq!(attribute(&schema, "Service.put", "maxDynamicData").as_deref(), Some("64"));
    assert_eq!(attribute(&schema, "Service.put", "tuple").as_deref(), Some("named"));
    // A method's argument list takes the same cap, and so does its Func.
    let attr = |name: &str, value: &str| Attribute { name: name.into(), value: Some(value.into()) };
    assert_eq!(schema.interfaces[0].funcs[0].attributes, [attr("maxFields", "4"), attr("maxDynamicData", "64")]);
    assert!(schema.interfaces[0].funcs[1].attributes.is_empty());
    dynamic::validate_schema(&schema).unwrap();
    // The cap changes no byte.
    assert_eq!(psio::to_pssz(&Capped { a: 1, b: None }), psio::to_pssz(&(1u32, None::<String>)));

    // A schema-driven reader refuses a record with more members than its
    // maxFields, more than 255 members, or a maxFields above 255.
    let record = |members: usize, max_fields: Option<&str>| {
        AnyType::new(V::Object(schema_ir::Object {
            members: (0..members)
                .map(|i| Member {
                    name: format!("m{i}"),
                    r#type: Box::new(AnyType::new(V::Int(schema_ir::Int { bits: 8, isSigned: false }))),
                    attributes: vec![],
                })
                .collect(),
            attributes: max_fields.map(|n| attr("maxFields", n)).into_iter().collect(),
        }))
    };
    // Any schema-driven entry point starts by checking the schema.
    let reads = |root: &AnyType| dynamic::auto_width(&Schema::default(), root).is_ok();
    assert!(reads(&record(3, None)));
    assert!(reads(&record(3, Some("3"))));
    assert!(reads(&record(3, Some("255"))));
    assert!(!reads(&record(4, Some("3"))));
    assert!(!reads(&record(3, Some("256"))));
    assert!(!reads(&record(3, Some("many"))));
    assert!(reads(&record(255, None)));
    assert!(!reads(&record(256, None)));
    // A Struct has the same limits.
    let fixed = AnyType::new(V::Struct(schema_ir::Struct {
        members: (0..256)
            .map(|i| Member {
                name: format!("m{i}"),
                r#type: Box::new(AnyType::new(V::Int(schema_ir::Int { bits: 8, isSigned: false }))),
                attributes: vec![],
            })
            .collect(),
        attributes: vec![],
    }));
    assert!(!reads(&fixed));

    // validate_schema: a function's parameter list is a tuple.
    let with_params = |params: usize, attributes: Vec<Attribute>| {
        let mut out = schema.clone();
        out.interfaces.push(schema_ir::Interface {
            name: "api".into(),
            type_names: vec![],
            funcs: vec![schema_ir::Func {
                name: "call".into(),
                params: (0..params)
                    .map(|i| Member {
                        name: format!("p{i}"),
                        r#type: Box::new(AnyType::new(V::Int(schema_ir::Int { bits: 32, isSigned: false }))),
                        attributes: vec![],
                    })
                    .collect(),
                result: None,
                attributes,
            }],
            attributes: vec![],
        });
        out
    };
    assert!(dynamic::validate_schema(&with_params(255, vec![])).is_ok());
    assert!(dynamic::validate_schema(&with_params(256, vec![])).is_err());
    assert!(dynamic::validate_schema(&with_params(2, vec![attr("maxFields", "2")])).is_ok());
    assert!(dynamic::validate_schema(&with_params(3, vec![attr("maxFields", "2")])).is_err());
    assert!(dynamic::validate_schema(&with_params(1, vec![attr("maxFields", "256")])).is_err());
    assert!(dynamic::validate_schema(&with_params(1, vec![attr("maxDynamicData", "x")])).is_err());
    // A malformed `tuple` attribute is a malformed schema.
    let mut bad = record(1, None);
    if let V::Object(object) = &mut bad.value {
        object.attributes.push(attr("tuple", "sometimes"));
    }
    assert!(!reads(&bad));
}

// ── A value past a bound has no key (item 5) ────────────────────────────────

#[derive(Debug, Clone, PartialEq, PartialOrd, psio::PsszPack, psio::PsszUnpack, psio::Key)]
struct Wrappers {
    name: BoundedString<4>,
    raw: BoundedRawString<3>,
    list: BoundedList<u16, 2>,
    nested: Vec<Option<BoundedList<BoundedString<2>, 2>>>,
    map: psio::pssz::BoundedMap<u8, BoundedString<2>, 2>,
    set: psio::pssz::BoundedSet<u8, 2>,
}

#[test]
fn a_key_writer_refuses_what_a_key_reader_rejects() {
    let good = || Wrappers {
        name: BoundedString("abcd".into()),
        raw: BoundedRawString(vec![0xff, 0, 1]),
        list: BoundedList(vec![1, 2]),
        nested: vec![None, Some(BoundedList(vec![BoundedString("ab".into())]))],
        map: psio::pssz::BoundedMap(BTreeMap::from([(1, BoundedString("ab".into()))])),
        set: psio::pssz::BoundedSet(BTreeSet::from([1, 2])),
    };
    let key = try_to_key(&good()).unwrap();
    assert_eq!(from_key::<Wrappers>(&key).unwrap(), good());
    let refused = |what: &str, change: &dyn Fn(&mut Wrappers)| {
        let mut bad = good();
        change(&mut bad);
        assert!(try_to_key(&bad).is_err(), "{what}: written");
        let mut out = vec![7u8];
        assert!(psio::key::to_key_into(&bad, &mut out).is_err(), "{what}");
        assert_eq!(out, [7], "{what}: the output is left as it was");
        // The key an unchecked writer produces is rejected by the readers.
        let unchecked = key_bytes(&bad);
        assert!(validate_key::<Wrappers>(&unchecked).is_err(), "{what}: validated");
        assert!(from_key::<Wrappers>(&unchecked).is_err(), "{what}: read");
        // The value has no pSSZ encoding either.
        assert!(psio::try_to_pssz(&bad).is_err(), "{what}: pssz");
    };
    refused("a string over its bound", &|w| w.name = BoundedString("abcde".into()));
    refused("a raw string over its bound", &|w| w.raw = BoundedRawString(vec![1, 2, 3, 4]));
    refused("a list over its bound", &|w| w.list = BoundedList(vec![1, 2, 3]));
    refused("an inner list over its bound", &|w| {
        w.nested = vec![Some(BoundedList(vec![BoundedString::new(), BoundedString::new(), BoundedString::new()]))]
    });
    refused("an inner string over its bound", &|w| w.nested = vec![Some(BoundedList(vec![BoundedString("abc".into())]))]);
    refused("a map over its bound", &|w| {
        w.map = psio::pssz::BoundedMap((0..3).map(|k| (k, BoundedString::new())).collect())
    });
    refused("a map value over its bound", &|w| {
        w.map = psio::pssz::BoundedMap(BTreeMap::from([(1, BoundedString("abc".into()))]))
    });
    refused("a set over its bound", &|w| w.set = psio::pssz::BoundedSet(BTreeSet::from([1, 2, 3])));
}

// ── A transparent type that bounds its member (item 13) ─────────────────────

#[derive(
    Debug, Clone, PartialEq, Eq, PartialOrd, Ord, psio::PsszPack, psio::PsszUnpack, psio::JsonText,
    psio::ToPsszSchema, psio::Key,
)]
#[psio(transparent)]
struct Memo {
    #[psio(max = 80)]
    text: String,
}

#[test]
fn a_transparent_type_that_bounds_its_member_is_that_bounded_type() {
    assert_eq!(<Memo as PsszPack<Pssz>>::INFERRED_BOUND, Some(80));
    assert_eq!(<Memo as PsszPack<Pssz>>::MAX_ENCODED_SIZE, Some(80));
    assert_eq!(auto_pssz_width::<Vec<Memo>>(), auto_pssz_width::<Vec<BoundedString<80>>>());
    let memo = Memo { text: "hi".into() };
    assert_eq!(psio::to_pssz(&memo), b"hi");
    assert_eq!(json::try_encode::<Json, _>(&memo).unwrap(), "\"hi\"");
    assert_eq!(try_to_key(&memo).unwrap(), try_to_key(&"hi".to_string()).unwrap());
    // The schema is the bounded string's, under the wrapper's name: a
    // named alias of Custom{BoundedList{u8, 80}, "utf8"}.
    let (schema, root) = schema_ir::schema_for::<Memo>().unwrap();
    assert_eq!(root, AnyType::named("Memo"));
    match &schema.resolve(&root).unwrap().value {
        V::Custom(custom) => {
            assert_eq!(custom.id, "utf8");
            match &custom.r#type.value {
                V::BoundedList(list) => {
                    assert_eq!(list.maxCount, 80);
                    assert_eq!(
                        schema.resolve(&list.r#type).unwrap().value,
                        V::Int(schema_ir::Int { bits: 8, isSigned: false })
                    );
                }
                other => panic!("not a bounded list: {other:?}"),
            }
        }
        other => panic!("not the bounded string: {other:?}"),
    }
    // Over the bound: no writer writes it and no reader reads it, bare, as
    // a list element, an optional payload, a map key and a map value.
    let long = Memo { text: "m".repeat(81) };
    assert!(psio::try_to_pssz(&long).is_err());
    assert!(json::try_encode::<Json, _>(&long).is_err());
    assert!(try_to_key(&long).is_err());
    assert!(psio::try_to_pssz(&vec![long.clone()]).is_err());
    assert!(psio::try_to_pssz(&Some(long.clone())).is_err());
    assert!(psio::try_to_pssz(&BTreeMap::from([(long.clone(), 1u8)])).is_err());
    assert!(psio::try_to_pssz(&BTreeMap::from([(1u8, long.clone())])).is_err());
    let frame = "m".repeat(81).into_bytes();
    assert!(psio::from_pssz::<Memo>(&frame).is_err());
    assert!(psio::validate_pssz::<Memo>(&frame).is_err());
    assert!(psio::from_pssz::<Memo>(&[0xff]).is_err());
    assert!(json::decode::<Json, Memo>(format!("\"{}\"", "m".repeat(81))).is_err());
    assert!(from_key::<Memo>(&key_bytes(&"m".repeat(81))).is_err());
    assert!(psio::from_pssz::<Memo>(&"m".repeat(80).into_bytes()).is_ok());
}

// ── Strict validation and raw strings (item 19) ─────────────────────────────

#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema)]
struct Strings {
    raw: RawString,
    text: String,
    inner: psio::pssz::Nested<RawString>,
}

#[test]
fn strict_validation_takes_a_raw_string_that_is_not_utf8() {
    let value = Strings {
        raw: RawString(vec![0xff, 0xc0, 0x80]),
        text: "ok".into(),
        inner: psio::pssz::Nested::new(&RawString(vec![0xfe])).unwrap(),
    };
    let frame = psio::to_pssz(&value);
    assert!(validate_strict::<Pssz, Strings>(&frame).is_ok());
    assert_eq!(decode_strict::<Pssz, Strings>(&frame).unwrap(), value);
    let (schema, root) = schema_ir::schema_for::<Strings>().unwrap();
    assert_eq!(dynamic::validate(&schema, &root, &frame), Ok(false));
    assert!(dynamic::decode_exact::<Pssz>(&schema, &root, &frame).is_ok());
    // A UTF-8 string is still text, strict or not.
    let bad = psio::to_pssz(&(RawString(vec![b'a']), RawString(vec![0xff]), psio::pssz::Nested::<RawString>::new(&RawString(vec![])).unwrap()));
    assert!(psio::validate_pssz::<Strings>(&bad).is_err());
    assert!(validate_strict::<Pssz, Strings>(&bad).is_err());
    assert!(dynamic::validate(&schema, &root, &bad).is_err());
}

// ── Reserved custom ids on the wrong wire shape (item 3) ────────────────────

#[test]
fn a_reserved_id_on_another_wire_shape_is_a_malformed_schema() {
    let int = |bits: u32| AnyType::new(V::Int(schema_ir::Int { bits, isSigned: false }));
    let custom = |inner: AnyType, id: &str| {
        AnyType::new(V::Custom(schema_ir::Custom { r#type: Box::new(inner), id: id.into() }))
    };
    let list = |inner: AnyType| AnyType::new(V::List(schema_ir::List { r#type: Box::new(inner) }));
    let reads = |root: &AnyType| dynamic::auto_width(&Schema::default(), root).is_ok();
    // The built-in shapes read.
    assert!(reads(&custom(int(1), "bool")));
    assert!(reads(&custom(list(int(8)), "string")));
    assert!(reads(&custom(list(int(8)), "utf8")));
    assert!(reads(&custom(int(64), "time-point-seconds")));
    // The same ids on any other shape do not: the id would be read as the
    // built-in type.
    assert!(!reads(&custom(list(int(8)), "bool")));
    assert!(!reads(&custom(int(8), "bool")));
    assert!(!reads(&custom(int(32), "string")));
    assert!(!reads(&custom(list(int(16)), "utf8")));
    assert!(!reads(&custom(custom(list(int(8)), "string"), "time-point-seconds")));
    assert!(!reads(&custom(int(64), "time-point-fortnights")));
    assert!(!reads(&custom(int(64), "duration-")));
    assert!(!reads(&custom(int(1), "set")));
    assert!(!reads(&custom(int(8), "unknown")));
}
