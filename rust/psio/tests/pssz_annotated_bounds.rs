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

// A list's own offset table follows its own bound (docs/pssz-spec.md 3.1),
// and a record's cap bounds what lies beneath it (3.6): the fixture's types.
pair!(ListW1 / WrapListW1 "ListW1" {
    #[psio(max = 15, element(max = 16))]
    items: Vec<RawString> => BoundedList<BoundedRawString<16>, 15>
});
pair!(ListFree / WrapListFree "ListFree" {
    #[psio(max = 8)]
    items: Vec<RawString> => BoundedList<RawString, 8>
});
pair!(ListNested / WrapListNested "ListNested" {
    #[psio(max = 20, element(max = 15, element(max = 16)))]
    rows: Vec<Vec<RawString>> => BoundedList<BoundedList<BoundedRawString<16>, 15>, 20>
});
pair!(MapW1 / WrapMapW1 "MapW1" {
    #[psio(max = 4, keys(max = 8))]
    m: BTreeMap<RawString, u32> => BoundedMap<BoundedRawString<8>, u32, 4>
});
pair!(MapW2 / WrapMapW2 "MapW2" {
    #[psio(max = 100, values(max = 16))]
    m: BTreeMap<u32, RawString> => BoundedMap<u32, BoundedRawString<16>, 100>
});
pair!(SetW1 / WrapSetW1 "SetW1" {
    #[psio(max = 15, element(max = 16))]
    s: BTreeSet<RawString> => BoundedSet<BoundedRawString<16>, 15>
});
macro_rules! capped {
    ($cap:literal $name:ident { $($(#[$meta:meta])* $field:ident : $ty:ty),* }) => {
        #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, ToPsszSchema, PsszView)]
        #[pssz(max_dynamic_data = $cap)]
        pub struct $name { $($(#[$meta])* pub $field: $ty),* }
    };
}
capped!(255 CapList { items: Vec<RawString> });
capped!(255 CapNested { rows: Vec<Vec<RawString>> });
capped!(255 CapMap { m: BTreeMap<RawString, Vec<u32>> });
capped!(255 CapTuple { t: (RawString, Vec<RawString>) });
capped!(255 CapTransfer { token: u32, from: u64, amount: u64, memo: String, data: Option<Vec<u8>> });
capped!(60000 CapTight { #[psio(max = 8, element(max = 16))] items: Vec<RawString> });
capped!(255 CapClamp { #[psio(max = 1000, element(max = 1000))] items: Vec<RawString> });

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
        assert_eq!(A::INFERRED_BOUND, W::INFERRED_BOUND);
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
    // conformance/pssz-vectors.json: auto_width and inferred_bound.
    fn bound<T: PsszPack<Pssz>>() -> (usize, Option<usize>) {
        (auto_pssz_width::<T>(), T::INFERRED_BOUND)
    }
    assert_eq!(bound::<AnnTags>(), (1, Some(138)));
    assert_eq!(bound::<AnnRawTags>(), (1, Some(138)));
    assert_eq!(bound::<AnnGrid>(), (1, Some(29)));
    assert_eq!(bound::<AnnOpt>(), (1, Some(37)));
    assert_eq!(bound::<AnnArr>(), (1, Some(53)));
    assert_eq!(bound::<AnnMap>(), (1, Some(114)));
    assert_eq!(bound::<AnnSet>(), (1, Some(70)));
    assert_eq!(bound::<AnnTup>(), (1, Some(66)));
    // The annotated list has the list's own width: a 1-byte offset table.
    assert_eq!(psio::to_pssz(&AnnTags { tags: vec![s("ab"), s("c")] }), [1, 1, 2, 4, b'a', b'b', b'c']);
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
    // Narrow list tables and position caps change no schema: the annotated
    // member is the bounded type, the member beneath a cap the plain one.
    same::<ListW1>(&fixture, "ListW1");
    same::<WrapListW1>(&fixture, "ListW1");
    same::<ListFree>(&fixture, "ListFree");
    same::<ListNested>(&fixture, "ListNested");
    same::<MapW1>(&fixture, "MapW1");
    same::<MapW2>(&fixture, "MapW2");
    same::<SetW1>(&fixture, "SetW1");
    same::<CapList>(&fixture, "CapList");
    same::<CapNested>(&fixture, "CapNested");
    same::<CapMap>(&fixture, "CapMap");
    same::<CapTuple>(&fixture, "CapTuple");
    same::<CapTransfer>(&fixture, "CapTransfer");
    same::<CapTight>(&fixture, "CapTight");
    same::<CapClamp>(&fixture, "CapClamp");
}

/// The fixture's pinned bytes, written from plain `Vec` / `BTreeMap` /
/// `BTreeSet` members; the annotation and the wrapper spelling agree, and
/// the schema-driven codec reads and writes the same bytes from the IR
/// Rust builds.
#[test]
fn list_tables_follow_their_own_bound_and_the_cap_above_them() {
    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|b| format!("{b:02x}")).collect()
    }
    fn pinned<T>(value: &T, expected: &str)
    where
        T: PsszPack<Pssz> + PsszUnpack<Pssz> + psio::pssz::schema_ir::ToPsszSchema + PartialEq + std::fmt::Debug,
    {
        let bytes = psio::to_pssz(value);
        assert_eq!(hex(&bytes), expected);
        assert_eq!(&psio::from_pssz::<T>(&bytes).unwrap(), value);
        assert_eq!(&psio::from_pssz_trusted::<T>(&bytes), value);
        let (schema, root) = schema_for::<T>().unwrap();
        let dynamic = psio::pssz::dynamic::decode_exact::<Pssz>(&schema, &root, &bytes).unwrap();
        assert_eq!(hex(&psio::pssz::dynamic::encode::<Pssz>(&schema, &root, &dynamic).unwrap()), expected);
    }
    let r = |text: &str| RawString::from(text);
    let two = || vec![r("ab"), r("c")];
    pinned(&ListW1 { items: two() }, "020002000204616263");
    pinned(&WrapListW1 { items: BoundedList(vec![BoundedRawString(b"ab".to_vec()), BoundedRawString(b"c".to_vec())]) }, "020002000204616263");
    pinned(&ListFree { items: two() }, "0400000004000000080000000a000000616263");
    pinned(&ListNested { rows: vec![two(), vec![]] }, "02000200040009000204616263");
    pinned(&SetW1 { s: two().into_iter().collect() }, "020002000204616263");
    pinned(&CapList { items: two() }, "01010204616263");
    pinned(&CapNested { rows: vec![two(), vec![]] }, "010102070204616263");
    pinned(&CapTuple { t: (r("x"), two()) }, "0101020203780204616263");
    pinned(&CapTight { items: two() }, "020002000204616263");
    pinned(&CapClamp { items: two() }, "01010204616263");
    pinned(
        &CapTransfer { token: 1, from: 2, amount: 3, memo: "hi".into(), data: Some(vec![0xaa]) },
        "1601000000020000000000000003000000000000001618686901aa",
    );
    assert_eq!(auto_pssz_width::<BoundedList<BoundedString<16>, 15>>(), 1);
    assert_eq!(auto_pssz_width::<BoundedList<BoundedString<17>, 15>>(), 2);
    assert_eq!(auto_pssz_width::<BoundedList<String, 8>>(), 4);

    // Views read what lies beneath a cap at the widths the cap selects: the
    // member's view carries the position in its format tag.
    let bytes = psio::to_pssz(&CapNested { rows: vec![two(), vec![r("z")]] });
    let view = psio::PsszView::<CapNested>::new(&bytes).unwrap();
    let rows: psio::PsszView<'_, Vec<Vec<RawString>>, psio::pssz::PsszUnder<255>> = view.rows();
    assert_eq!(rows.len(), 2);
    assert_eq!(rows.get(0).get(1).view().as_bytes(), b"c");
    assert_eq!(rows.get(1).get(0).view().as_bytes(), b"z");
    let bytes = psio::to_pssz(&CapTuple { t: (r("x"), two()) });
    let view = psio::PsszView::<CapTuple>::new(&bytes).unwrap();
    assert_eq!(view.t().field::<1>().get(0).view().as_bytes(), b"ab");
    let bytes = psio::to_pssz(&CapMap { m: BTreeMap::from([(r("k"), vec![7u32])]) });
    assert_eq!(hex(&bytes), "0101010202036b07000000");
    let view = psio::PsszView::<CapMap>::new(&bytes).unwrap();
    assert_eq!(view.m().get(0).value().get(0).get(), 7);
    // And a mutable view writes through the same widths.
    let mut bytes = bytes;
    {
        use CapMapPsszMutAccessors;
        let mut view = psio::PsszMutView::<CapMap>::new(&mut bytes).unwrap();
        view.m_mut().get_mut(0).value_mut().get_mut(0).set(&9).unwrap();
    }
    assert_eq!(psio::from_pssz::<CapMap>(&bytes).unwrap().m[&r("k")], vec![9]);
}

/// A transparent type's member bound is the type's, in every format and on
/// every entry point; so is the bound of an enum alternative.
#[test]
fn a_transparent_member_bound_is_enforced_everywhere() {
    #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, ToPsszSchema, JsonText, Key, PsszView)]
    #[psio(transparent)]
    struct Memo(#[psio(max = 80)] String);
    #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, JsonText, Key)]
    enum Note {
        Short(#[psio(max = 4)] String),
        Number(u32),
    }
    #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, JsonText, Key)]
    struct Holder {
        id: u8,
        memo: Memo,
    }
    let at = Memo("m".repeat(80));
    let over = Memo("m".repeat(81));
    // Keys: the writer, the reader and the validator.
    let key = psio::try_to_key(&at).unwrap();
    assert_eq!(psio::from_key::<Memo>(&key).unwrap(), at);
    assert_eq!(psio::validate_key::<Memo>(&key), Ok(()));
    assert!(psio::try_to_key(&over).is_err());
    let long = psio::to_key(&over.0);
    assert_eq!(psio::from_key::<String>(&long).unwrap(), over.0);
    assert!(psio::from_key::<Memo>(&long).is_err());
    assert!(psio::validate_key::<Memo>(&long).is_err());
    // Inside a record's key too.
    assert!(psio::try_to_key(&Holder { id: 1, memo: over.clone() }).is_err());
    let holder_key = psio::to_key(&(1u8, over.0.clone()));
    assert!(psio::from_key::<Holder>(&holder_key).is_err());
    assert!(psio::validate_key::<Holder>(&holder_key).is_err());
    assert!(psio::from_key::<Holder>(&psio::to_key(&(1u8, at.0.clone()))).is_ok());
    // An alternative's bound.
    assert!(psio::try_to_key(&Note::Short("abcd".into())).is_ok());
    assert!(psio::try_to_key(&Note::Short("abcde".into())).is_err());
    let mut alt = vec![0u8];
    alt.extend(psio::to_key(&"abcde".to_string()));
    assert!(psio::from_key::<Note>(&alt).is_err() && psio::validate_key::<Note>(&alt).is_err());
    // pSSZ and JSON agree; the view and the schema are the bounded string's.
    assert!(psio::try_to_pssz(&over).is_err());
    assert!(psio::from_pssz::<Memo>(over.0.as_bytes()).is_err());
    assert!(psio::PsszView::<Memo>::new(over.0.as_bytes()).is_err());
    assert!(psio::PsszView::<Memo>::new(at.0.as_bytes()).is_ok());
    assert!(psio::try_to_json(&over).is_err());
    assert!(psio::from_json::<Memo>(&format!("\"{}\"", over.0)).is_err());
    assert_eq!(psio::from_json::<Memo>(&format!("\"{}\"", at.0)).unwrap(), at);
    // `Memo` names the bounded UTF-8 string in the schema.
    let schema = format!("{:?}", psio::pssz::schema_ir::schema_for::<Memo>().unwrap().0);
    assert!(schema.contains("maxCount: 80") && schema.contains("id: \"utf8\""), "{schema}");
}
