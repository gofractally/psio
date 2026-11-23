#![allow(dead_code)]
// The Rust port of the variant cases of
// cpp/tests/pssz_evolution_matrix_tests.cpp: a variant may gain
// alternatives at the end (docs/pssz-spec.md 7.5), and a larger new
// alternative is refused exactly when it moves an enclosing width. A C++
// `std::variant<...>` is a Rust enum with data.

use psio::pssz::{
    auto_pssz_width, from_pssz, inferred_bound, pssz_evolves_from as evolves, to_pssz, validate, BoundedList, Pssz,
    PsszPack, PsszValidate,
};
use std::collections::BTreeMap;

macro_rules! derives {
    ($($item:item)*) => {$(
        #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack)]
        $item
    )*};
}
derives! {
    #[pssz(max_dynamic_data = 64)]
    struct Alt0 { a: u32 }
    #[pssz(max_dynamic_data = 64)]
    struct Alt1 { a: u32, b: Option<u32> }
    #[pssz(max_dynamic_data = 64)]
    struct AltOther { z: u64 }

    enum V1<A> { A(A) }
    enum V2<A, B> { A(A), B(B) }
    enum V3<A, B, C> { A(A), B(B), C(C) }
    enum V4<A, B, C, D> { A(A), B(B), C(C), D(D) }

    // Bounds on the alternatives.
    enum AnnV0 { A(u32), B(#[psio(max = 8)] String) }
    enum AnnV1 { A(u32), B(#[psio(max = 8)] String), C(#[psio(max = 4)] String) }
    enum AnnVTighter { A(u32), B(#[psio(max = 4)] String), C(#[psio(max = 4)] String) }
    enum AnnVWider { A(u32), B(#[psio(max = 16)] String), C(#[psio(max = 4)] String) }
}

// The records that hold a variant, one set per variant type (a derived
// record with a cap is not generic over its members).
macro_rules! holders {
    ($($v:ty => $tight:ident, $capped:ident, $listed:ident;)*) => {$(derives! {
        // A record with no cap: its width is the class of its inferred
        // bound, which contains the variant's (1 + the largest alternative).
        //   header 1 + slot 1 + (1 + 2) + 249 = 254: one byte wide.
        struct $tight { v: $v, pad: [u8; 249] }
        // The same members under a cap: the cap rules the width.
        #[pssz(max_dynamic_data = 1000)]
        struct $capped { v: $v, pad: [u8; 249] }
        // A bounded list of variants inside a capped record: the record's
        // width is the cap's, and the list's own table is what a larger
        // alternative moves (60 x (1 + 3) = 240, one byte; 60 x (2 + 5) =
        // 420, two bytes).
        #[pssz(max_dynamic_data = 60000)]
        struct $listed { #[psio(max = 60)] items: Vec<$v> }
    })*};
}
holders! {
    Small => TightSmall, CappedSmall, ListedSmall;
    Same => TightSame, CappedSame, ListedSame;
    Larger => TightLarger, CappedLarger, ListedLarger;
    V2<u8, Alt0> => TightOld, CappedOld, ListedOld;
    V3<u8, Alt1, u64> => TightNew, CappedNew, ListedNew;
}
macro_rules! annotated {
    ($($v:ty => $name:ident;)*) => {$(derives! {
        #[pssz(max_dynamic_data = 200)]
        struct $name { v: $v }
    })*};
}
annotated! {
    AnnV0 => Ann0;
    AnnV1 => Ann1;
    AnnVTighter => AnnTighter;
    AnnVWider => AnnWider;
}

type Var1 = V2<u32, String>;
type Var2 = V3<u32, String, u64>;
type Var3 = V4<u32, String, u64, Alt0>;
type Small = V2<u8, u16>;
type Same = V3<u8, u16, u8>; // a new alternative no larger than the others
type Larger = V3<u8, u16, u32>; // a new alternative that raises the bound by 2
type Pad = [u8; 249];

const _: () = {
    assert!(matches!(inferred_bound::<TightSmall>(), Some(254)) && auto_pssz_width::<TightSmall>() == 1);
    assert!(matches!(inferred_bound::<TightSame>(), Some(254)) && auto_pssz_width::<TightSame>() == 1);
    assert!(matches!(inferred_bound::<TightLarger>(), Some(258)) && auto_pssz_width::<TightLarger>() == 2);
    assert!(auto_pssz_width::<CappedSmall>() == 2 && auto_pssz_width::<CappedLarger>() == 2);
    assert!(auto_pssz_width::<ListedSmall>() == 2 && auto_pssz_width::<ListedLarger>() == 2);
    assert!(auto_pssz_width::<BoundedList<Small, 60>>() == 1 && auto_pssz_width::<BoundedList<Larger, 60>>() == 2);
};

#[test]
fn a_variant_may_gain_alternatives_at_the_end() {
    // Usable in const context.
    const _: () = assert!(evolves::<Var1, Var2>());
    // Appended, once and twice; the relation is transitive.
    assert!(evolves::<Var1, Var2>());
    assert!(evolves::<Var2, Var3>());
    assert!(evolves::<Var1, Var3>());
    assert!(evolves::<Var1, Var1>());
    // Removed, reordered, inserted, retyped: not compatible.
    assert!(!evolves::<Var2, Var1>());
    assert!(!evolves::<Var1, V3<String, u32, u64>>());
    assert!(!evolves::<Var1, V3<u32, u64, String>>());
    assert!(!evolves::<Var1, V3<u64, u32, String>>());
    assert!(!evolves::<Var1, V3<u64, String, u32>>());
    assert!(!evolves::<Var1, V3<i32, String, u64>>());
    // An existing alternative evolves by the rules of its own type, with or
    // without new alternatives after it.
    assert!(evolves::<V2<u32, Alt0>, V2<u32, Alt1>>());
    assert!(evolves::<V2<u32, Alt0>, V3<u32, Alt1, u64>>());
    assert!(!evolves::<V2<u32, Alt1>, V3<u32, Alt0, u64>>());
    assert!(!evolves::<V2<u32, Alt0>, V3<u32, AltOther, u64>>());
    // Wherever a variant is: an optional, a list, a map, a variant, a record.
    assert!(evolves::<Option<Var1>, Option<Var2>>());
    assert!(evolves::<Vec<Var1>, Vec<Var2>>());
    assert!(evolves::<BTreeMap<u32, Var1>, BTreeMap<u32, Var2>>());
    assert!(evolves::<V2<u8, Var1>, V3<u8, Var2, u16>>());
    assert!(!evolves::<Vec<Var2>, Vec<Var1>>());
}

#[test]
fn a_larger_new_alternative_may_not_move_an_enclosing_width() {
    // The variant alone writes no offsets: appending is compatible whatever
    // the size of the new alternative.
    assert!(evolves::<Small, Same>());
    assert!(evolves::<Small, Larger>());
    assert!(evolves::<V1<u8>, V2<u8, [u8; 300]>>());

    // A record with no cap: refused exactly when its width moves, as for an
    // appended field.
    assert!(evolves::<TightSmall, TightSame>());
    assert!(!evolves::<TightSmall, TightLarger>());
    // A cap reserves the room: the same change is compatible.
    assert!(evolves::<CappedSmall, CappedLarger>());
    assert!(evolves::<CappedSmall, CappedSame>());

    // A tuple and an array take their width from their own bound.
    assert!(evolves::<(Small, Pad), (Same, Pad)>());
    assert!(!evolves::<(Small, Pad), (Larger, Pad)>());
    assert!(evolves::<[Small; 60], [Same; 60]>());
    assert!(!evolves::<[Small; 60], [Larger; 60]>());
    // A bounded list on its own, and a bounded map's table.
    assert!(evolves::<BoundedList<Small, 60>, BoundedList<Same, 60>>());
    assert!(!evolves::<BoundedList<Small, 60>, BoundedList<Larger, 60>>());

    // A bounded list inside a capped record: the record's width stays, the
    // list's table does not.
    assert!(evolves::<ListedSmall, ListedSame>());
    assert!(!evolves::<ListedSmall, ListedLarger>());
}

#[test]
fn bounds_of_a_variants_alternatives() {
    assert!(evolves::<Ann0, Ann1>());
    assert!(evolves::<Ann0, AnnWider>());
    assert!(!evolves::<Ann0, AnnTighter>());
    assert!(!evolves::<Ann1, Ann0>());
}

fn valid<T: PsszPack<Pssz> + PsszValidate<Pssz>>(bytes: &[u8]) -> bool {
    validate::<Pssz, T>(bytes).is_ok()
}

#[test]
fn an_appended_alternative_the_bytes_agree_with_the_check() {
    // New reads everything Old wrote, to the same value and the same bytes.
    for old_value in [Var1::A(7), Var1::B("hello".into()), Var1::B(String::new())] {
        let bytes = to_pssz(&old_value);
        assert!(valid::<Var2>(&bytes));
        let as_new: Var2 = from_pssz(&bytes).unwrap();
        match (&old_value, &as_new) {
            (V2::A(a), V3::A(b)) => assert_eq!(a, b),
            (V2::B(a), V3::B(b)) => assert_eq!(a, b),
            other => panic!("{other:?}"),
        }
        assert_eq!(to_pssz(&as_new), bytes);
        // ... and Old reads a New value that is a value of Old's type.
        assert!(valid::<Var1>(&to_pssz(&as_new)));
    }
    // Old rejects the alternative it does not know.
    let added = to_pssz(&Var2::C(9));
    assert!(valid::<Var2>(&added));
    assert!(!valid::<Var1>(&added));
    assert!(from_pssz::<Var1>(&added).is_err());

    // Inside a record, with the alternative itself evolved.
    type Old = CappedOld;
    type New = CappedNew;
    assert!(evolves::<Old, New>());
    let record_bytes = to_pssz(&Old { v: V2::B(Alt0 { a: 5 }), pad: [0; 249] });
    assert!(valid::<New>(&record_bytes));
    let new_record: New = from_pssz(&record_bytes).unwrap();
    assert_eq!(new_record.v, V3::B(Alt1 { a: 5, b: None }));
    assert_eq!(to_pssz(&new_record), record_bytes);
    let grown = New { v: V3::C(1), pad: [0; 249] };
    assert!(!valid::<Old>(&to_pssz(&grown)));
}
