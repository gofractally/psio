//! What a type declares about its values is enforced by the validating read
//! paths of the formats whose bytes follow another specification:
//! FlatBuffers, Cap'n Proto, protobuf (`psio::contract`, C++
//! `psio/value_contract.hpp`; gofractally/psio#14). A value outside a
//! member's bound has the same verdict in every format.
//!
//! Each buffer is written from a twin type — the same members without the
//! declarations, what a writer that does not know them produces — and read
//! as the declared type.

use psio::flatbuf::{FbLimits, FbOptions, FbUnpack, FbView};
use psio::protobuf::PbMessage;
use std::collections::BTreeMap;

const FB: &str = "flatbuf: member length_bound violated";
const PB: &str = "protobuf: member length_bound violated";
const CP: &str = "capnp: member length_bound violated";
const UNSORTED: &str = "sorted: elements are not in order";
const NOT_ASCENDING: &str = "sorted: elements are not in strictly ascending order";

/// A declared type and its twin, in the three formats.
macro_rules! twin {
    ($loose:ident, $declared:ident, [$($ty:tt)*], $(#[$ann:meta])*) => {
        #[derive(Debug, Clone, PartialEq, Default)]
        #[derive(psio::FbPack, psio::FbUnpack, psio::FbView, psio::CapnpPack, psio::CapnpUnpack, psio::Protobuf)]
        struct $loose {
            v: $($ty)*,
        }
        #[derive(Debug, Clone, PartialEq, Default)]
        #[derive(psio::FbPack, psio::FbUnpack, psio::FbView, psio::CapnpPack, psio::CapnpUnpack, psio::Protobuf)]
        struct $declared {
            $(#[$ann])*
            v: $($ty)*,
        }
    };
}
/// The same without Cap'n Proto (whose Rust mapping has no maps).
macro_rules! twin_map {
    ($loose:ident, $declared:ident, [$($ty:tt)*], $(#[$ann:meta])*) => {
        #[derive(Debug, Clone, PartialEq, Default, psio::FbPack, psio::FbUnpack, psio::FbView, psio::Protobuf)]
        struct $loose {
            v: $($ty)*,
        }
        #[derive(Debug, Clone, PartialEq, Default, psio::FbPack, psio::FbUnpack, psio::FbView, psio::Protobuf)]
        struct $declared {
            $(#[$ann])*
            v: $($ty)*,
        }
    };
}

twin!(MaxListLoose, MaxList, [Vec<u32>], #[psio(max = 2)]);
twin!(MaxStringLoose, MaxString, [String], #[psio(max = 2)]);
twin!(MaxBytesLoose, MaxBytes, [Vec<u8>], #[psio(max = 2)]);
twin!(ElementLoose, Element, [Vec<String>], #[psio(element(max = 2))]);
twin!(SortedLoose, Sorted, [Vec<u32>], #[psio(sorted)]);
twin!(UniqueLoose, Unique, [Vec<u32>], #[psio(unique)]);
twin_map!(MaxMapLoose, MaxMap, [BTreeMap<String, String>], #[psio(max = 1)]);
twin_map!(KeysLoose, Keys, [BTreeMap<String, String>], #[psio(keys(max = 1))]);
twin_map!(ValuesLoose, Values, [BTreeMap<String, String>], #[psio(values(max = 1))]);
twin_map!(OptionalLoose, Optional, [Option<String>], #[psio(max = 2)]);

#[derive(Debug, Clone, PartialEq, Default)]
#[derive(psio::FbPack, psio::FbUnpack, psio::FbView, psio::CapnpPack, psio::CapnpUnpack, psio::Protobuf)]
struct EvenLoose {
    v: u32,
}
#[derive(Debug, Clone, PartialEq, Default)]
#[derive(psio::FbPack, psio::FbUnpack, psio::FbView, psio::CapnpPack, psio::CapnpUnpack, psio::Protobuf)]
#[psio(invariant = "even")]
struct Even {
    v: u32,
}
fn even(e: &Even) -> Result<(), &'static str> {
    if e.v % 2 == 0 {
        Ok(())
    } else {
        Err("Even: v is odd")
    }
}

/// An invariant whose message is computed from the value.
#[derive(Debug, Clone, PartialEq, Default)]
#[derive(psio::FbPack, psio::FbUnpack, psio::CapnpPack, psio::CapnpUnpack, psio::Protobuf)]
#[psio(invariant = "small")]
struct Small {
    v: u32,
}
fn small(s: &Small) -> Result<(), String> {
    if s.v < 10 {
        Ok(())
    } else {
        Err(format!("Small: {} is not below 10", s.v))
    }
}

/// A declaration below a record that declares nothing itself.
#[derive(Debug, Clone, PartialEq, Default)]
#[derive(psio::FbPack, psio::FbUnpack, psio::FbView, psio::CapnpPack, psio::CapnpUnpack, psio::Protobuf)]
struct OuterLoose {
    rows: Vec<MaxListLoose>,
    one: EvenLoose,
}
#[derive(Debug, Clone, PartialEq, Default)]
#[derive(psio::FbPack, psio::FbUnpack, psio::FbView, psio::CapnpPack, psio::CapnpUnpack, psio::Protobuf)]
struct Outer {
    rows: Vec<MaxList>,
    one: Even,
}

/// A type that holds itself, with and without a declaration.
#[derive(Debug, Clone, PartialEq, Default, psio::FbPack, psio::FbUnpack, psio::FbView, psio::Protobuf)]
struct TreeLoose {
    label: String,
    children: Vec<TreeLoose>,
}
#[derive(Debug, Clone, PartialEq, Default, psio::FbPack, psio::FbUnpack, psio::FbView, psio::Protobuf)]
struct Tree {
    label: String,
    #[psio(max = 2)]
    children: Vec<Tree>,
}

/// An alternative of a union / oneof that declares something.
#[derive(Debug, Clone, PartialEq, psio::FbPack, psio::FbUnpack, psio::Protobuf)]
enum ChoiceLoose {
    None,
    List(MaxListLoose),
    Even(EvenLoose),
}
#[derive(Debug, Clone, PartialEq, psio::FbPack, psio::FbUnpack, psio::Protobuf)]
enum Choice {
    None,
    List(MaxList),
    Even(Even),
}
#[derive(Debug, Clone, PartialEq, psio::FbPack, psio::FbUnpack, psio::Protobuf)]
struct HolderLoose {
    choice: ChoiceLoose,
}
#[derive(Debug, Clone, PartialEq, psio::FbPack, psio::FbUnpack, psio::Protobuf)]
struct Holder {
    choice: Choice,
}

/// A record with a type parameter.
#[derive(Debug, Clone, PartialEq, psio::FbPack, psio::FbUnpack)]
struct Wrap<T> {
    inner: T,
    #[psio(max = 1)]
    notes: Vec<u32>,
}

// ── FlatBuffers ─────────────────────────────────────────────────────────────

/// `bad` is refused with `why` by the verifier, the checked decode and the
/// validating view; `good` is accepted by all three.
macro_rules! flatbuf {
    ($declared:ty, $bad:expr, $good:expr, $why:expr) => {{
        let (bad, good) = (psio::to_flatbuf(&$bad), psio::to_flatbuf(&$good));
        let e = psio::validate_flatbuf::<$declared>(&bad).expect_err("validate");
        assert_eq!((e.message(), e.offset()), ($why, 0));
        assert_eq!(psio::from_flatbuf::<$declared>(&bad).expect_err("decode").message(), $why);
        assert_eq!(<$declared as FbUnpack>::fb_unpack(&bad).expect_err("unpack").message(), $why);
        assert_eq!(
            psio::validate_flatbuf_with::<$declared>(&bad, &FbOptions::default(), FbLimits::default())
                .expect_err("validate_with")
                .message(),
            $why
        );
        assert!(<$declared as FbUnpack>::fb_has_declarations());
        psio::validate_flatbuf::<$declared>(&good).expect("validate");
        psio::from_flatbuf::<$declared>(&good).expect("decode");
        // The trusted decode checks nothing, as before.
        let _ = psio::from_flatbuf_trusted::<$declared>(&bad);
    }};
}
macro_rules! flatbuf_view {
    ($declared:ty, $bad:expr, $good:expr, $why:expr) => {{
        let (bad, good) = (psio::to_flatbuf(&$bad), psio::to_flatbuf(&$good));
        assert_eq!(FbView::<$declared>::new(&bad).map(|_| ()).expect_err("view").message(), $why);
        assert_eq!(
            FbView::<$declared>::new_with(&bad, &FbOptions::default(), FbLimits::default())
                .map(|_| ())
                .expect_err("view")
                .message(),
            $why
        );
        FbView::<$declared>::new(&good).expect("view");
        let _ = FbView::<$declared>::trusted(&bad);
    }};
}

fn text(s: &str) -> String {
    s.to_string()
}
fn map(entries: &[(&str, &str)]) -> BTreeMap<String, String> {
    entries.iter().map(|(k, v)| (text(k), text(v))).collect()
}

#[test]
fn flatbuf_refuses_a_value_outside_what_its_type_declares() {
    flatbuf!(MaxList, MaxListLoose { v: vec![1, 2, 3] }, MaxListLoose { v: vec![1, 2] }, FB);
    flatbuf_view!(MaxList, MaxListLoose { v: vec![1, 2, 3] }, MaxListLoose { v: vec![1, 2] }, FB);
    flatbuf!(MaxString, MaxStringLoose { v: text("abc") }, MaxStringLoose { v: text("ab") }, FB);
    flatbuf_view!(MaxString, MaxStringLoose { v: text("abc") }, MaxStringLoose { v: text("ab") }, FB);
    flatbuf!(MaxBytes, MaxBytesLoose { v: vec![1, 2, 3] }, MaxBytesLoose { v: vec![1, 2] }, FB);
    flatbuf!(Element, ElementLoose { v: vec![text("ab"), text("abc")] }, ElementLoose { v: vec![text("ab")] }, FB);
    flatbuf_view!(Element, ElementLoose { v: vec![text("abc")] }, ElementLoose { v: vec![] }, FB);
    flatbuf!(Sorted, SortedLoose { v: vec![2, 1] }, SortedLoose { v: vec![1, 1, 2] }, UNSORTED);
    flatbuf_view!(Sorted, SortedLoose { v: vec![2, 1] }, SortedLoose { v: vec![1, 1, 2] }, UNSORTED);
    flatbuf!(Unique, UniqueLoose { v: vec![1, 1] }, UniqueLoose { v: vec![1, 2] }, NOT_ASCENDING);
    flatbuf!(MaxMap, MaxMapLoose { v: map(&[("a", "b"), ("c", "d")]) }, MaxMapLoose { v: map(&[("a", "b")]) }, FB);
    flatbuf!(Keys, KeysLoose { v: map(&[("ab", "b")]) }, KeysLoose { v: map(&[("a", "bbb")]) }, FB);
    flatbuf!(Values, ValuesLoose { v: map(&[("a", "bc")]) }, ValuesLoose { v: map(&[("aaa", "b")]) }, FB);
    flatbuf_view!(Values, ValuesLoose { v: map(&[("a", "bc")]) }, ValuesLoose { v: map(&[("aaa", "b")]) }, FB);
    flatbuf!(Optional, OptionalLoose { v: Some(text("abc")) }, OptionalLoose { v: Some(text("ab")) }, FB);
    flatbuf!(Optional, OptionalLoose { v: Some(text("abc")) }, OptionalLoose { v: None }, FB);
    flatbuf!(Even, EvenLoose { v: 3 }, EvenLoose { v: 4 }, "Even: v is odd");
    flatbuf_view!(Even, EvenLoose { v: 3 }, EvenLoose { v: 4 }, "Even: v is odd");
    // A message the invariant computed is not carried by `FbError`.
    flatbuf!(Small, Small { v: 11 }, Small { v: 9 }, "flatbuf: invariant violated");
}

#[test]
fn flatbuf_follows_everything_a_value_holds() {
    let row = |v: &[u32]| MaxListLoose { v: v.to_vec() };
    let outer = |rows: Vec<MaxListLoose>, one: u32| OuterLoose { rows, one: EvenLoose { v: one } };
    flatbuf!(Outer, outer(vec![row(&[1]), row(&[1, 2, 3])], 0), outer(vec![row(&[1]), row(&[1, 2])], 0), FB);
    flatbuf_view!(Outer, outer(vec![row(&[1, 2, 3])], 0), outer(vec![], 0), FB);
    flatbuf!(Outer, outer(vec![], 5), outer(vec![], 6), "Even: v is odd");
    // The first violation in declaration order.
    flatbuf!(Outer, outer(vec![row(&[1, 2, 3])], 5), outer(vec![], 0), FB);

    // A type that holds itself: the bound holds at every depth.
    let leaf = |label: &str| TreeLoose { label: text(label), children: vec![] };
    let wide = TreeLoose { label: text("root"), children: vec![leaf("a"), leaf("b"), leaf("c")] };
    let deep = TreeLoose { label: text("root"), children: vec![leaf("a"), wide.clone()] };
    let fine = TreeLoose { label: text("root"), children: vec![leaf("a"), leaf("b")] };
    flatbuf!(Tree, wide, fine, FB);
    flatbuf!(Tree, deep, fine, FB);
    flatbuf_view!(Tree, deep, fine, FB);

    // The alternative a union holds.
    let holder = |choice| HolderLoose { choice };
    flatbuf!(Holder, holder(ChoiceLoose::List(row(&[1, 2, 3]))), holder(ChoiceLoose::List(row(&[1]))), FB);
    flatbuf!(Holder, holder(ChoiceLoose::Even(EvenLoose { v: 1 })), holder(ChoiceLoose::None), "Even: v is odd");

    // A record with a type parameter.
    let wrapped = |v: &[u32], notes: &[u32]| Wrap { inner: row(v), notes: notes.to_vec() };
    flatbuf!(Wrap<MaxList>, wrapped(&[1, 2, 3], &[]), wrapped(&[1, 2], &[9]), FB);
    flatbuf!(Wrap<MaxListLoose>, wrapped(&[1, 2, 3], &[1, 2]), wrapped(&[1, 2, 3], &[1]), FB);
}

#[test]
fn a_type_that_declares_nothing_is_not_decoded_to_be_verified() {
    assert!(!MaxListLoose::fb_has_declarations());
    assert!(!OuterLoose::fb_has_declarations());
    assert!(!TreeLoose::fb_has_declarations()); // holds itself
    assert!(!HolderLoose::fb_has_declarations());
    assert!(Tree::fb_has_declarations());
    assert!(Outer::fb_has_declarations());
    assert!(Holder::fb_has_declarations());
    assert!(Wrap::<u32>::fb_has_declarations());
    // The answer is the type's, whichever type was asked first.
    assert!(Even::fb_has_declarations() && MaxList::fb_has_declarations());

    assert!(!MaxListLoose::pb_has_declarations());
    assert!(!OuterLoose::pb_has_declarations());
    assert!(!TreeLoose::pb_has_declarations());
    assert!(!HolderLoose::pb_has_declarations());
    assert!(Tree::pb_has_declarations());
    assert!(Outer::pb_has_declarations());
    assert!(Holder::pb_has_declarations());
}

#[test]
fn flatbuf_checks_the_structure_first_and_reads_the_prefixed_forms() {
    let mut bytes = psio::to_flatbuf(&MaxListLoose { v: vec![1, 2, 3] });
    bytes.truncate(bytes.len() - 2);
    let loose = psio::validate_flatbuf::<MaxListLoose>(&bytes).expect_err("truncated");
    let declared = psio::validate_flatbuf::<MaxList>(&bytes).expect_err("truncated");
    assert_eq!(declared, loose);
    assert_ne!(declared.message(), FB);

    let opts = FbOptions { file_identifier: Some(*b"BNDD"), size_prefixed: true };
    let bad = psio::to_flatbuf_with(&MaxListLoose { v: vec![1, 2, 3] }, &opts);
    let good = psio::to_flatbuf_with(&MaxListLoose { v: vec![1, 2] }, &opts);
    assert_eq!(
        psio::validate_flatbuf_with::<MaxList>(&bad, &opts, FbLimits::default()).expect_err("prefixed").message(),
        FB
    );
    assert!(FbView::<MaxList>::new_with(&bad, &opts, FbLimits::default()).is_err());
    psio::validate_flatbuf_with::<MaxList>(&good, &opts, FbLimits::default()).expect("prefixed");
    assert_eq!(FbView::<MaxList>::new_with(&good, &opts, FbLimits::default()).unwrap().unpack().v, [1, 2]);
}

// ── protobuf ────────────────────────────────────────────────────────────────

macro_rules! protobuf {
    ($declared:ty, $bad:expr, $good:expr, $why:expr) => {{
        let (bad, good) = (psio::to_protobuf(&$bad), psio::to_protobuf(&$good));
        let e = psio::validate_protobuf::<$declared>(&bad).expect_err("validate");
        assert_eq!((e.message(), e.offset()), ($why, 0));
        assert_eq!(psio::from_protobuf::<$declared>(&bad).expect_err("decode").message(), $why);
        assert!(<$declared as PbMessage>::pb_has_declarations());
        psio::validate_protobuf::<$declared>(&good).expect("validate");
        psio::from_protobuf::<$declared>(&good).expect("decode");
    }};
}

#[test]
fn protobuf_refuses_a_value_outside_what_its_type_declares() {
    protobuf!(MaxList, MaxListLoose { v: vec![1, 2, 3] }, MaxListLoose { v: vec![1, 2] }, PB);
    protobuf!(MaxString, MaxStringLoose { v: text("abc") }, MaxStringLoose { v: text("ab") }, PB);
    protobuf!(MaxBytes, MaxBytesLoose { v: vec![1, 2, 3] }, MaxBytesLoose { v: vec![1, 2] }, PB);
    protobuf!(Element, ElementLoose { v: vec![text("ab"), text("abc")] }, ElementLoose { v: vec![text("ab")] }, PB);
    protobuf!(Sorted, SortedLoose { v: vec![2, 1] }, SortedLoose { v: vec![1, 1, 2] }, UNSORTED);
    protobuf!(Unique, UniqueLoose { v: vec![1, 1] }, UniqueLoose { v: vec![1, 2] }, NOT_ASCENDING);
    protobuf!(MaxMap, MaxMapLoose { v: map(&[("a", "b"), ("c", "d")]) }, MaxMapLoose { v: map(&[("a", "b")]) }, PB);
    protobuf!(Keys, KeysLoose { v: map(&[("ab", "b")]) }, KeysLoose { v: map(&[("a", "bbb")]) }, PB);
    protobuf!(Values, ValuesLoose { v: map(&[("a", "bc")]) }, ValuesLoose { v: map(&[("aaa", "b")]) }, PB);
    protobuf!(Optional, OptionalLoose { v: Some(text("abc")) }, OptionalLoose { v: None }, PB);
    protobuf!(Even, EvenLoose { v: 3 }, EvenLoose { v: 4 }, "Even: v is odd");
    protobuf!(Small, Small { v: 11 }, Small { v: 9 }, "protobuf: invariant violated");

    let row = |v: &[u32]| MaxListLoose { v: v.to_vec() };
    let outer = |rows: Vec<MaxListLoose>, one: u32| OuterLoose { rows, one: EvenLoose { v: one } };
    protobuf!(Outer, outer(vec![row(&[1]), row(&[1, 2, 3])], 0), outer(vec![row(&[1, 2])], 0), PB);
    protobuf!(Outer, outer(vec![], 5), outer(vec![], 6), "Even: v is odd");
    let leaf = |label: &str| TreeLoose { label: text(label), children: vec![] };
    let wide = TreeLoose { label: text("root"), children: vec![leaf("a"), leaf("b"), leaf("c")] };
    let deep = TreeLoose { label: text("root"), children: vec![leaf("a"), wide.clone()] };
    protobuf!(Tree, deep, leaf("root"), PB);
    let holder = |choice| HolderLoose { choice };
    protobuf!(Holder, holder(ChoiceLoose::List(row(&[1, 2, 3]))), holder(ChoiceLoose::List(row(&[1]))), PB);
    protobuf!(Holder, holder(ChoiceLoose::Even(EvenLoose { v: 1 })), holder(ChoiceLoose::None), "Even: v is odd");
}

#[test]
fn protobuf_holds_the_declarations_against_the_finished_value() {
    // `v` (field 1) written twice: the last string is the value.
    let replaced = [0x0a, 3, b'a', b'b', b'c', 0x0a, 2, b'o', b'k'];
    psio::validate_protobuf::<MaxString>(&replaced).expect("the last value is inside the bound");
    assert_eq!(psio::from_protobuf::<MaxString>(&replaced).unwrap().v, "ok");
    let replaced_bad = [0x0a, 2, b'o', b'k', 0x0a, 3, b'a', b'b', b'c'];
    assert_eq!(psio::validate_protobuf::<MaxString>(&replaced_bad).expect_err("over").message(), PB);
    // A packed list written in two parts is one list.
    let two_parts = [0x0a, 2, 1, 2, 0x0a, 1, 3];
    psio::validate_protobuf::<MaxListLoose>(&two_parts).expect("loose");
    assert_eq!(psio::validate_protobuf::<MaxList>(&two_parts).expect_err("three").message(), PB);
    // The structure first.
    let mut bytes = psio::to_protobuf(&MaxListLoose { v: vec![1, 2, 3] });
    bytes.pop();
    assert_eq!(
        psio::validate_protobuf::<MaxList>(&bytes).expect_err("truncated"),
        psio::validate_protobuf::<MaxListLoose>(&bytes).expect_err("truncated")
    );
}

// ── Cap'n Proto ─────────────────────────────────────────────────────────────

macro_rules! capnp {
    ($declared:ty, $bad:expr, $good:expr, $why:expr) => {{
        let (bad, good) = (psio::to_capnp(&$bad), psio::to_capnp(&$good));
        assert_eq!(psio::validate_capnp::<$declared>(&bad).expect_err("validate"), $why);
        assert_eq!(psio::from_capnp::<$declared>(&bad).expect_err("decode"), $why);
        assert_eq!(<$declared as psio::capnp::CapnpUnpack>::capnp_unpack(&bad).expect_err("unpack"), $why);
        psio::validate_capnp::<$declared>(&good).expect("validate");
        psio::from_capnp::<$declared>(&good).expect("decode");
    }};
}

#[test]
fn capnp_refuses_a_value_outside_what_its_type_declares() {
    capnp!(MaxList, MaxListLoose { v: vec![1, 2, 3] }, MaxListLoose { v: vec![1, 2] }, CP);
    capnp!(MaxString, MaxStringLoose { v: text("abc") }, MaxStringLoose { v: text("ab") }, CP);
    capnp!(MaxBytes, MaxBytesLoose { v: vec![1, 2, 3] }, MaxBytesLoose { v: vec![1, 2] }, CP);
    capnp!(Element, ElementLoose { v: vec![text("ab"), text("abc")] }, ElementLoose { v: vec![text("ab")] }, CP);
    capnp!(Sorted, SortedLoose { v: vec![2, 1] }, SortedLoose { v: vec![1, 1, 2] }, UNSORTED);
    capnp!(Unique, UniqueLoose { v: vec![1, 1] }, UniqueLoose { v: vec![1, 2] }, NOT_ASCENDING);
    capnp!(Even, EvenLoose { v: 3 }, EvenLoose { v: 4 }, "Even: v is odd");
    capnp!(Small, Small { v: 11 }, Small { v: 9 }, "capnp: invariant violated");
    let row = |v: &[u32]| MaxListLoose { v: v.to_vec() };
    let outer = |rows: Vec<MaxListLoose>, one: u32| OuterLoose { rows, one: EvenLoose { v: one } };
    capnp!(Outer, outer(vec![row(&[1]), row(&[1, 2, 3])], 0), outer(vec![row(&[1, 2])], 0), CP);
    capnp!(Outer, outer(vec![], 5), outer(vec![], 6), "Even: v is odd");
}

// ── One declaration, one verdict ────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq, Default)]
#[derive(psio::PsszPack, psio::PsszUnpack, psio::JsonText, psio::Key)]
#[derive(psio::FbPack, psio::FbUnpack, psio::CapnpPack, psio::CapnpUnpack, psio::Protobuf)]
struct Everywhere {
    #[psio(max = 2)]
    ids: Vec<u32>,
}
#[derive(Debug, Clone, PartialEq, Default)]
#[derive(psio::PsszPack, psio::PsszUnpack, psio::JsonText, psio::Key)]
#[derive(psio::FbPack, psio::FbUnpack, psio::CapnpPack, psio::CapnpUnpack, psio::Protobuf)]
struct EverywhereLoose {
    ids: Vec<u32>,
}

#[test]
fn the_verdict_is_the_one_of_pssz_key_and_json() {
    let over = Everywhere { ids: vec![1, 2, 3] };
    let loose = EverywhereLoose { ids: vec![1, 2, 3] };
    assert!(psio::try_to_pssz(&over).is_err());
    assert!(psio::try_to_key(&over).is_err());
    assert!(psio::try_to_json(&over).is_err());
    assert!(psio::from_key::<Everywhere>(&psio::to_key(&loose)).is_err());
    assert!(psio::from_json::<Everywhere>(&psio::to_json(&loose)).is_err());
    assert!(psio::from_flatbuf::<Everywhere>(&psio::to_flatbuf(&loose)).is_err());
    assert!(psio::from_capnp::<Everywhere>(&psio::to_capnp(&loose)).is_err());
    assert!(psio::from_protobuf::<Everywhere>(&psio::to_protobuf(&loose)).is_err());
    let within = EverywhereLoose { ids: vec![1, 2] };
    assert_eq!(psio::from_flatbuf::<Everywhere>(&psio::to_flatbuf(&within)).unwrap().ids, [1, 2]);
    assert_eq!(psio::from_capnp::<Everywhere>(&psio::to_capnp(&within)).unwrap().ids, [1, 2]);
    assert_eq!(psio::from_protobuf::<Everywhere>(&psio::to_protobuf(&within)).unwrap().ids, [1, 2]);
}
