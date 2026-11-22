//! Bounds by annotation (docs/pssz-spec.md 3.5; RG6): a member keeps its
//! plain type, `#[psio(max = N, element(...), keys(...), values(...),
//! at(I, ...))]` bounds it, and the result is the type the bounded wrapper
//! types spell: the same bytes in every profile, the same widths, the same
//! Schema IR, the same refusals.

use psio::pssz::schema_ir::schema_for;
use psio::pssz::{
    auto_pssz_width, BoundedList, BoundedMap, BoundedRawString, BoundedSet, BoundedString, Pssz, Pssz16, Pssz32,
    Pssz8, PsszFormat, PsszPack, PsszUnpack, PsszValidate, RawString,
};
use psio::{JsonText, Key, PsszView, ToPsszSchema};
use std::collections::{BTreeMap, BTreeSet};

// Each pair: the annotation spelling and the wrapper spelling of one type.
// The wrapper carries the annotated record's schema name so the two IRs
// can be compared whole.
macro_rules! pair {
    ($ann:ident / $wrap:ident $name:literal { $(#[$meta:meta])* $field:ident : $plain:ty => $bounded:ty }) => {
        #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, ToPsszSchema, JsonText)]
        pub struct $ann {
            $(#[$meta])*
            pub $field: $plain,
        }
        #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, ToPsszSchema, JsonText)]
        #[psio(name = $name)]
        pub struct $wrap {
            pub $field: $bounded,
        }
    };
}

pair!(AnnTags / WrapTags "AnnTags" {
    #[psio(max = 8, element(max = 16))]
    tags: Vec<String> => BoundedList<BoundedString<16>, 8>
});
pair!(AnnRawTags / WrapRawTags "AnnRawTags" {
    #[psio(max = 8, element(max = 16))]
    tags: Vec<RawString> => BoundedList<BoundedRawString<16>, 8>
});
pair!(AnnGrid / WrapGrid "AnnGrid" {
    #[psio(max = 3, element(max = 4))]
    rows: Vec<Vec<u16>> => BoundedList<BoundedList<u16, 4>, 3>
});
pair!(AnnOpt / WrapOpt "AnnOpt" {
    #[psio(max = 2, element(max = 16))]
    names: Option<Vec<String>> => Option<BoundedList<BoundedString<16>, 2>>
});
pair!(AnnArr / WrapArr "AnnArr" {
    #[psio(element(max = 16))]
    names: [String; 3] => [BoundedString<16>; 3]
});
pair!(AnnMap / WrapMap "AnnMap" {
    #[psio(max = 4, keys(max = 16), values(max = 2))]
    m: BTreeMap<String, Vec<u32>> => BoundedMap<BoundedString<16>, BoundedList<u32, 2>, 4>
});
pair!(AnnSet / WrapSet "AnnSet" {
    #[psio(max = 4, element(max = 16))]
    s: BTreeSet<String> => BoundedSet<BoundedString<16>, 4>
});
pair!(AnnCount / WrapCount "AnnCount" {
    #[psio(max = 2)]
    m: BTreeMap<u8, RawString> => BoundedMap<u8, RawString, 2>
});
pair!(AnnDeep / WrapDeep "AnnDeep" {
    #[psio(element(element(at(1, max = 3, element(max = 2)))))]
    deep: Vec<Vec<(u8, Vec<String>)>> => Vec<Vec<(u8, BoundedList<BoundedString<2>, 3>)>>
});

#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, ToPsszSchema, JsonText)]
pub struct AnnTup {
    #[psio(at(0, max = 16), at(2, max = 5))]
    pub t: (String, u32, Vec<u8>),
    #[psio(at(0, max = 16), at(1, max = 16))]
    pub p: (String, String),
}
#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, ToPsszSchema, JsonText)]
#[psio(name = "AnnTup")]
pub struct WrapTup {
    pub t: (BoundedString<16>, u32, BoundedList<u8, 5>),
    pub p: (BoundedString<16>, BoundedString<16>),
}

// A variant's alternatives are annotated where they are declared.
#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, ToPsszSchema, JsonText)]
pub enum AnnChoice {
    Number(u32),
    Text(#[psio(max = 16)] String),
    List(#[psio(max = 2, element(max = 16))] Vec<String>),
    Both(#[psio(max = 4)] String, #[psio(max = 2)] Vec<u8>),
    Named {
        #[psio(max = 4)]
        label: String,
    },
}
#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, ToPsszSchema, JsonText)]
#[psio(name = "AnnChoice")]
pub enum WrapChoice {
    Number(u32),
    Text(BoundedString<16>),
    List(BoundedList<BoundedString<16>, 2>),
    Both(BoundedString<4>, BoundedList<u8, 2>),
    Named { label: BoundedString<4> },
}
pair!(AnnVar / WrapVar "AnnVar" { v: AnnChoice => WrapChoice });

// A transparent type over a bounded member: psibase's Memo.
#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, ToPsszSchema, JsonText, Key)]
#[psio(transparent)]
pub struct Memo(#[psio(max = 80)] pub String);
pair!(AnnMemo / WrapMemo "AnnMemo" { memo: Memo => BoundedString<80> });

trait Codec<F: PsszFormat>: PsszPack<F> + PsszUnpack<F> + PsszValidate<F> {}
impl<F: PsszFormat, T: PsszPack<F> + PsszUnpack<F> + PsszValidate<F>> Codec<F> for T {}
trait All: Codec<Pssz> + Codec<Pssz8> + Codec<Pssz16> + Codec<Pssz32> + psio::pssz::schema_ir::ToPsszSchema + psio::json::JsonText + std::fmt::Debug {}
impl<T> All for T where
    T: Codec<Pssz> + Codec<Pssz8> + Codec<Pssz16> + Codec<Pssz32> + psio::pssz::schema_ir::ToPsszSchema + psio::json::JsonText + std::fmt::Debug
{
}

/// The two spellings are one type: constants, bytes in every profile, the
/// Schema IR, and each reads what the other writes.
fn same_type<A: All + PartialEq, W: All>(ann: &A, wrap: &W) {
    fn consts<F: PsszFormat, A: PsszPack<F>, W: PsszPack<F>>() {
        assert_eq!(A::AUTO_BOUND, W::AUTO_BOUND);
        assert_eq!(A::MAX_ENCODED_SIZE, W::MAX_ENCODED_SIZE);
        assert_eq!(A::OFFSET_WIDTH, W::OFFSET_WIDTH);
        assert_eq!(A::IS_FIXED_SIZE, W::IS_FIXED_SIZE);
        assert_eq!(A::MIN_ENCODED_SIZE, W::MIN_ENCODED_SIZE);
    }
    fn bytes<F: PsszFormat, A: Codec<F> + PartialEq + std::fmt::Debug, W: Codec<F>>(ann: &A, wrap: &W) {
        let a = psio::pssz::try_encode::<F, A>(ann).unwrap();
        let w = psio::pssz::try_encode::<F, W>(wrap).unwrap();
        assert_eq!(a, w, "{ann:?}");
        assert_eq!(ann.pssz_size(), a.len());
        A::pssz_validate(&w).unwrap();
        W::pssz_validate(&a).unwrap();
        assert_eq!(&A::pssz_unpack(&w).unwrap(), ann);
        assert_eq!(psio::pssz::try_encode::<F, W>(&W::pssz_unpack(&a).unwrap()).unwrap(), a);
    }
    consts::<Pssz, A, W>();
    consts::<Pssz8, A, W>();
    consts::<Pssz16, A, W>();
    consts::<Pssz32, A, W>();
    bytes::<Pssz, A, W>(ann, wrap);
    bytes::<Pssz8, A, W>(ann, wrap);
    bytes::<Pssz16, A, W>(ann, wrap);
    bytes::<Pssz32, A, W>(ann, wrap);
    // One Schema IR, whole.
    assert_eq!(schema_for::<A>().unwrap(), schema_for::<W>().unwrap());
    // One text.
    assert_eq!(psio::to_ijson(ann), psio::to_ijson(wrap));
    assert_eq!(psio::from_ijson::<A>(psio::to_ijson(wrap)).unwrap(), *ann);
    // The schema-driven codec reads the annotated type's bytes with its schema.
    let (schema, root) = schema_for::<A>().unwrap();
    let encoded = psio::to_pssz(ann);
    psio::pssz::dynamic::validate(&schema, &root, &encoded).unwrap();
    let (text, _) = psio::pssz::dynamic::json::pssz_to_json::<psio::IJson>(&schema, &root, &encoded).unwrap();
    assert_eq!(text, psio::to_ijson(ann));
}

fn s(text: &str) -> String {
    text.to_string()
}
fn b(text: &str) -> BoundedString<16> {
    BoundedString(text.to_string())
}

#[test]
fn an_annotated_member_is_the_bounded_type() {
    same_type(&AnnTags { tags: vec![s("a"), s("bc")] }, &WrapTags { tags: BoundedList(vec![b("a"), b("bc")]) });
    same_type(&AnnTags { tags: vec![] }, &WrapTags { tags: BoundedList(vec![]) });
    same_type(
        &AnnTags { tags: vec!["x".repeat(16); 8] },
        &WrapTags { tags: BoundedList(vec![b(&"x".repeat(16)); 8]) },
    );
    same_type(
        &AnnRawTags { tags: vec![RawString(b"ab".to_vec())] },
        &WrapRawTags { tags: BoundedList(vec![BoundedRawString(b"ab".to_vec())]) },
    );
    same_type(
        &AnnGrid { rows: vec![vec![1, 2], vec![], vec![3, 4, 5, 6]] },
        &WrapGrid { rows: BoundedList(vec![BoundedList(vec![1, 2]), BoundedList(vec![]), BoundedList(vec![3, 4, 5, 6])]) },
    );
    // Raw bytes that are not UTF-8 are a value in pSSZ (and have no text).
    assert_eq!(
        psio::to_pssz(&AnnRawTags { tags: vec![RawString(vec![0xff, 0xfe])] }),
        psio::to_pssz(&WrapRawTags { tags: BoundedList(vec![BoundedRawString(vec![0xff, 0xfe])]) })
    );
    same_type(&AnnOpt { names: None }, &WrapOpt { names: None });
    same_type(
        &AnnOpt { names: Some(vec![s("al"), s("bo")]) },
        &WrapOpt { names: Some(BoundedList(vec![b("al"), b("bo")])) },
    );
    same_type(
        &AnnArr { names: [s("a"), s(""), s("ccc")] },
        &WrapArr { names: [b("a"), b(""), b("ccc")] },
    );
    same_type(
        &AnnMap { m: [(s("k"), vec![1, 2]), (s("l"), vec![])].into() },
        &WrapMap { m: BoundedMap([(b("k"), BoundedList(vec![1, 2])), (b("l"), BoundedList(vec![]))].into()) },
    );
    same_type(&AnnSet { s: [s("x"), s("y")].into() }, &WrapSet { s: BoundedSet([b("x"), b("y")].into()) });
    same_type(
        &AnnCount { m: [(1, RawString(vec![1])), (2, RawString(vec![]))].into() },
        &WrapCount { m: BoundedMap([(1, RawString(vec![1])), (2, RawString(vec![]))].into()) },
    );
    same_type(
        &AnnDeep { deep: vec![vec![(1, vec![s("ab"), s("")])], vec![]] },
        &WrapDeep { deep: vec![vec![(1, BoundedList(vec![BoundedString(s("ab")), BoundedString(s(""))]))], vec![]] },
    );
    same_type(
        &AnnTup { t: (s("to"), 7, vec![1, 2, 3]), p: (s("a"), s("b")) },
        &WrapTup { t: (b("to"), 7, BoundedList(vec![1, 2, 3])), p: (b("a"), b("b")) },
    );
    for (ann, wrap) in [
        (AnnChoice::Number(7), WrapChoice::Number(7)),
        (AnnChoice::Text(s("t")), WrapChoice::Text(b("t"))),
        (AnnChoice::List(vec![s("a"), s("b")]), WrapChoice::List(BoundedList(vec![b("a"), b("b")]))),
        (
            AnnChoice::Both(s("ab"), vec![1]),
            WrapChoice::Both(BoundedString(s("ab")), BoundedList(vec![1])),
        ),
        (AnnChoice::Named { label: s("n") }, WrapChoice::Named { label: BoundedString(s("n")) }),
    ] {
        same_type(&AnnVar { v: ann }, &WrapVar { v: wrap });
    }
}

#[test]
fn the_bounds_select_the_widths_the_fixture_pins() {
    // conformance/pssz-vectors.json: auto_width and max_encoded_size.
    fn bound<T: PsszPack<Pssz>>() -> (usize, Option<usize>) {
        (auto_pssz_width::<T>(), T::MAX_ENCODED_SIZE)
    }
    assert_eq!(bound::<AnnTags>(), (1, Some(168)));
    assert_eq!(bound::<AnnRawTags>(), (1, Some(168)));
    assert_eq!(bound::<AnnGrid>(), (1, Some(44)));
    assert_eq!(bound::<AnnOpt>(), (1, Some(49)));
    assert_eq!(bound::<AnnArr>(), (1, Some(68)));
    assert_eq!(bound::<AnnMap>(), (1, Some(168)));
    assert_eq!(bound::<AnnSet>(), (1, Some(88)));
    assert_eq!(bound::<AnnTup>(), (1, Some(93)));
    // The same members without the annotations are unbounded.
    #[derive(psio::PsszPack)]
    #[allow(dead_code)]
    struct Plain {
        tags: Vec<String>,
    }
    assert_eq!(bound::<Plain>(), (4, None));
    // `tags_two` of the fixture, automatic profile.
    assert_eq!(
        psio::to_pssz(&AnnTags { tags: vec![s("ab"), s("cde")] }),
        psio::to_pssz(&WrapTags { tags: BoundedList(vec![b("ab"), b("cde")]) })
    );
}

/// One past each bound: refused by the encoders and, as a frame, by every
/// reader of the annotated type; the plain type takes the same frame.
#[test]
fn every_bound_is_enforced_at_every_depth() {
    fn refused<A: All, P: All>(over: &P) {
        fn one<F: PsszFormat, A: Codec<F>, P: Codec<F>>(over: &P) {
            // A writer that skips the checks produces this frame.
            let frame = psio::pssz::try_encode::<F, P>(over).unwrap();
            assert!(A::pssz_validate(&frame).is_err(), "validate accepts {frame:?}");
            assert!(A::pssz_unpack(&frame).is_err(), "decode accepts {frame:?}");
        }
        one::<Pssz32, A, P>(over);
        one::<Pssz16, A, P>(over);
    }
    // The plain mirrors: the same members with no bound.
    macro_rules! plain {
        ($name:ident { $field:ident : $ty:ty }) => {
            #[derive(Debug, psio::PsszPack, psio::PsszUnpack, ToPsszSchema, JsonText)]
            struct $name {
                $field: $ty,
            }
        };
    }
    plain!(PTags { tags: Vec<String> });
    plain!(PGrid { rows: Vec<Vec<u16>> });
    plain!(POpt { names: Option<Vec<String>> });
    plain!(PArr { names: [String; 3] });
    plain!(PMap { m: BTreeMap<String, Vec<u32>> });
    plain!(PSet { s: BTreeSet<String> });
    plain!(PDeep { deep: Vec<Vec<(u8, Vec<String>)>> });

    let long = "x".repeat(17);
    refused::<AnnTags, _>(&PTags { tags: vec![s(""); 9] });
    refused::<AnnTags, _>(&PTags { tags: vec![long.clone()] });
    refused::<AnnGrid, _>(&PGrid { rows: vec![vec![]; 4] });
    refused::<AnnGrid, _>(&PGrid { rows: vec![vec![0; 5]] });
    refused::<AnnOpt, _>(&POpt { names: Some(vec![s(""); 3]) });
    refused::<AnnOpt, _>(&POpt { names: Some(vec![long.clone()]) });
    refused::<AnnArr, _>(&PArr { names: [s(""), long.clone(), s("")] });
    refused::<AnnMap, _>(&PMap { m: (0..5).map(|i| (i.to_string(), vec![])).collect() });
    refused::<AnnMap, _>(&PMap { m: [(long.clone(), vec![])].into() });
    refused::<AnnMap, _>(&PMap { m: [(s("k"), vec![1, 2, 3])].into() });
    refused::<AnnSet, _>(&PSet { s: [long.clone()].into() });
    refused::<AnnSet, _>(&PSet { s: (0..5).map(|i| i.to_string()).collect() });
    refused::<AnnDeep, _>(&PDeep { deep: vec![vec![(0, vec![s("abc")])]] });
    refused::<AnnDeep, _>(&PDeep { deep: vec![vec![(0, vec![s(""); 4])]] });

    // The encoders refuse the value, in every profile, and say why.
    fn unencodable<A: All>(value: &A) {
        for error in [
            psio::pssz::try_encode::<Pssz, A>(value).unwrap_err(),
            psio::pssz::try_encode::<Pssz8, A>(value).unwrap_err(),
            psio::pssz::try_encode::<Pssz16, A>(value).unwrap_err(),
            psio::pssz::try_encode::<Pssz32, A>(value).unwrap_err(),
        ] {
            assert_eq!(error.message(), "pssz: member length_bound violated", "{value:?}");
        }
        // Text too, in both directions.
        assert!(psio::try_to_ijson(value).is_err(), "{value:?}");
        assert!(psio::try_to_json(value).is_err(), "{value:?}");
    }
    unencodable(&AnnTags { tags: vec![s(""); 9] });
    unencodable(&AnnTags { tags: vec![long.clone()] });
    unencodable(&AnnRawTags { tags: vec![RawString(vec![0; 17])] });
    unencodable(&AnnGrid { rows: vec![vec![]; 4] });
    unencodable(&AnnGrid { rows: vec![vec![0; 5]] });
    unencodable(&AnnOpt { names: Some(vec![long.clone()]) });
    unencodable(&AnnArr { names: [s(""), s(""), long.clone()] });
    unencodable(&AnnMap { m: [(long.clone(), vec![])].into() });
    unencodable(&AnnMap { m: [(s("k"), vec![1, 2, 3])].into() });
    unencodable(&AnnSet { s: [long.clone()].into() });
    unencodable(&AnnDeep { deep: vec![vec![(0, vec![s("abc")])]] });
    unencodable(&AnnTup { t: (long.clone(), 0, vec![]), p: (s(""), s("")) });
    unencodable(&AnnTup { t: (s(""), 0, vec![0; 6]), p: (s(""), s("")) });
    unencodable(&AnnTup { t: (s(""), 0, vec![]), p: (s(""), long.clone()) });
    unencodable(&AnnVar { v: AnnChoice::Text(long.clone()) });
    unencodable(&AnnVar { v: AnnChoice::List(vec![s(""); 3]) });
    unencodable(&AnnVar { v: AnnChoice::List(vec![long.clone()]) });
    unencodable(&AnnVar { v: AnnChoice::Both(s("abcde"), vec![]) });
    unencodable(&AnnVar { v: AnnChoice::Named { label: s("abcde") } });
    unencodable(&AnnMemo { memo: Memo("m".repeat(81)) });

    // Text that holds a value outside a bound is rejected by the reader.
    for text in [
        r#"{"tags":["","","","","","","","",""]}"#,
        r#"{"tags":["xxxxxxxxxxxxxxxxx"]}"#,
    ] {
        assert!(psio::from_ijson::<AnnTags>(text).is_err(), "{text}");
        assert!(psio::from_json::<AnnTags>(text).is_err(), "{text}");
        assert!(psio::from_ijson::<PTags>(text).is_ok(), "{text}");
    }
    assert!(psio::from_ijson::<AnnVar>(r#"{"v":{"Text":"xxxxxxxxxxxxxxxxx"}}"#).is_err());
    assert!(psio::from_ijson::<AnnVar>(r#"{"v":{"Named":{"label":"abcde"}}}"#).is_err());
    assert!(psio::from_ijson::<AnnVar>(r#"{"v":{"Named":{"label":"abcd"}}}"#).is_ok());
    assert!(psio::from_ijson::<AnnMemo>(&format!(r#"{{"memo":"{}"}}"#, "m".repeat(81))).is_err());
    assert!(psio::from_ijson::<AnnMemo>(&format!(r#"{{"memo":"{}"}}"#, "m".repeat(80))).is_ok());
}

#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, PsszView, Key)]
struct Keyed {
    id: u32,
    #[psio(max = 4)]
    name: String,
    #[psio(max = 2, element(max = 3))]
    tags: Vec<String>,
}

#[test]
fn keys_and_views_see_the_bounds() {
    let value = Keyed { id: 1, name: s("abcd"), tags: vec![s("x"), s("yz")] };
    let key = psio::to_key(&value);
    assert_eq!(psio::from_key::<Keyed>(&key).unwrap(), value);
    // The key writer refuses a value outside a bound; the reader a key that
    // holds one.
    #[derive(Key)]
    struct Unbounded {
        id: u32,
        name: String,
        tags: Vec<String>,
    }
    for over in [
        Unbounded { id: 1, name: s("abcde"), tags: vec![] },
        Unbounded { id: 1, name: s(""), tags: vec![s(""); 3] },
        Unbounded { id: 1, name: s(""), tags: vec![s("abcd")] },
    ] {
        let key = psio::to_key(&over);
        assert!(psio::from_key::<Keyed>(&key).is_err());
        assert!(psio::validate_key::<Keyed>(&key).is_err());
        let as_bounded = Keyed { id: over.id, name: over.name, tags: over.tags };
        assert!(psio::try_to_key(&as_bounded).is_err());
    }

    // A view of the record reads the member as the bounded type it is.
    let bytes = psio::to_pssz(&value);
    let view = psio::PsszView::<Keyed>::new(&bytes).unwrap();
    assert_eq!(view.id().get(), 1);
    assert_eq!(view.name().view().unwrap(), "abcd");
    assert_eq!(view.tags().len(), 2);
    // A frame outside the bounds has no checked view.
    #[derive(psio::PsszPack)]
    struct Wide {
        id: u32,
        name: String,
        tags: Vec<String>,
    }
    let over = psio::pssz::encode::<Pssz8, Wide>(&Wide { id: 1, name: s("abcde"), tags: vec![] });
    assert!(psio::pssz::PsszView::<Keyed, Pssz8>::new(&over).is_err());
}

// ── The Schema IR C++ writes for its annotated members ──────────────────────

use psio::pssz::schema_ir::{self, AnyType, AnyValue as V};

/// A node with every reference replaced by what it refers to.
fn resolved(schema: &schema_ir::Schema, ty: &AnyType) -> AnyType {
    let go = |ty: &AnyType| Box::new(resolved(schema, ty));
    let members = |members: &Vec<schema_ir::Member>| {
        members
            .iter()
            .map(|m| schema_ir::Member { name: m.name.clone(), r#type: go(&m.r#type), attributes: m.attributes.clone() })
            .collect()
    };
    AnyType::new(match &ty.value {
        V::Type(reference) => return resolved(schema, schema.get(&reference.r#type).expect("a known type")),
        V::Object(o) => V::Object(schema_ir::Object { members: members(&o.members), attributes: o.attributes.clone() }),
        V::Struct(o) => V::Struct(schema_ir::Struct { members: members(&o.members), attributes: o.attributes.clone() }),
        V::Variant(o) => {
            V::Variant(schema_ir::Variant { members: members(&o.members), attributes: o.attributes.clone() })
        }
        V::List(l) => V::List(schema_ir::List { r#type: go(&l.r#type) }),
        V::BoundedList(l) => V::BoundedList(schema_ir::BoundedList { r#type: go(&l.r#type), maxCount: l.maxCount }),
        V::Array(a) => V::Array(schema_ir::Array { r#type: go(&a.r#type), len: a.len }),
        V::Option(o) => V::Option(schema_ir::Optional { r#type: go(&o.r#type) }),
        V::Custom(c) => V::Custom(schema_ir::Custom { r#type: go(&c.r#type), id: c.id.clone() }),
        V::Tuple(t) => V::Tuple(schema_ir::Tuple { members: t.members.iter().map(|m| go(m)).collect() }),
        other => other.clone(),
    })
}

/// conformance/pssz-vectors.json records the IR C++ builds for `AnnTags`
/// (`attr(tags, max<8> | element(utf8 | max<16>))` on a
/// `std::vector<std::string>`) and the others. The Rust annotation gives
/// the same node.
#[test]
fn the_schema_of_an_annotated_member_is_the_one_cpp_writes() {
    let text = std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/../../conformance/pssz-vectors.json"))
        .unwrap();
    let fixture: serde_json::Value = serde_json::from_str(&text).unwrap();
    let hex = fixture["schema_ir"].as_str().unwrap();
    let bytes: Vec<u8> =
        (0..hex.len()).step_by(2).map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap()).collect();
    let fixture: schema_ir::Schema = psio::from_pssz(&bytes).unwrap();

    fn same<T: psio::pssz::schema_ir::ToPsszSchema>(fixture: &schema_ir::Schema, id: &str) {
        let (ours, root) = schema_for::<T>().unwrap();
        let theirs = fixture.get(id).unwrap_or_else(|| panic!("{id} is in the fixture"));
        assert_eq!(resolved(&ours, &root), resolved(fixture, theirs), "{id}");
    }
    same::<AnnTags>(&fixture, "AnnTags");
    same::<WrapTags>(&fixture, "WrapTags");
    same::<AnnTags>(&fixture, "WrapTags");
    same::<AnnRawTags>(&fixture, "AnnRawTags");
    same::<AnnGrid>(&fixture, "AnnGrid");
    same::<AnnOpt>(&fixture, "AnnOpt");
    same::<AnnArr>(&fixture, "AnnArr");
    same::<AnnMap>(&fixture, "AnnMap");
    same::<AnnSet>(&fixture, "AnnSet");
    same::<AnnTup>(&fixture, "AnnTup");
}
