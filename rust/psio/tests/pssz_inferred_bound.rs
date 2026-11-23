// The Rust port of cpp/tests/pssz_inferred_bound_tests.cpp: the inferred
// size bound of variants and tuples (docs/pssz-spec.md 3.2).
//   Variant(Ts...)  1 + max(inferred_bound(T_i))
//   Tuple(Ts...)    the extensible record of its elements: header + per
//                   element its payload (+ an offset slot when variable)
// with the header and every slot counted at the tuple's own width (`fit`).
// A bounded tuple / variant, and a record that holds one, gets one- or
// two-byte offsets instead of four; the typed codec and the schema-driven
// one agree. A C++ `std::pair` is the Rust 2-tuple. Bit types are bounded:
// ceil(N / 8) bytes, a bitlist one more.

use psio::pssz::schema_ir::{schema_for, ToPsszSchema};
use psio::pssz::{
    auto_pssz_width, decode_checked, dynamic, validate, validate_strict, Bitlist, Bitvector, BoundedList,
    BoundedString, Pssz, PsszPack, PsszUnpack, PsszValidate,
};
use std::collections::{BTreeMap, BTreeSet};

type Name = BoundedString<32>;

const fn bound<T: PsszPack<Pssz>>() -> Option<usize> {
    T::INFERRED_BOUND
}
const fn width<T: PsszPack<Pssz>>() -> usize {
    T::OFFSET_WIDTH
}

macro_rules! derives {
    ($($item:item)*) => {$(
        #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema)]
        $item
    )*};
}
derives! {
    #[psio(definition_will_not_change)]
    struct Point { x: u32, y: u32 }
    #[pssz(max_dynamic_data = 300)]
    struct Capped { text: String }
    struct Free { text: String }
    // C++ std::variant<...> of one, two and three alternatives.
    enum V1<A> { A(A) }
    enum V2<A, B> { A(A), B(B) }
    enum V3<A, B, C> { A(A), B(B), C(C) }
    // A record with a variant member, and one with a tuple member.
    struct Event { at: u64, what: V3<u8, Point, u64> }
    struct Call { method: u32, args: (u64, Name, u8) }
    struct Open { id: u32, what: V2<u8, Vec<u8>> }
    // flags is inline (2 bytes, no slot); seen has one slot and at most 3 bytes.
    struct BitFlags { id: u8, flags: Bitvector<12>, seen: Bitlist<12> }
    // A bitvector is a fixed field: a DWNC record of one is its bytes.
    #[psio(definition_will_not_change)]
    struct BitFixed { flags: Bitvector<12> }
    struct BitList2016 { seen: Bitlist<2016> }
    struct BitList2017 { seen: Bitlist<2017> }
}

// ── Variant: 1 + the widest alternative ─────────────────────────────────────
const _: () = {
    assert!(matches!(bound::<V1<u8>>(), Some(2)));
    assert!(matches!(bound::<V2<u8, u32>>(), Some(5)));
    assert!(matches!(bound::<V3<u64, u8, u16>>(), Some(9)));
    assert!(matches!(bound::<V2<u8, Point>>(), Some(9)));
    assert!(matches!(bound::<V2<u8, Name>>(), Some(33)));
    assert!(matches!(bound::<V2<u8, Option<u32>>>(), Some(6)));
    assert!(matches!(bound::<V2<u8, V2<u16, u64>>>(), Some(10)));
    // An alternative with an explicit cap contributes the cap.
    assert!(matches!(bound::<V2<u8, Capped>>(), Some(301)));
    // Unbounded if any alternative is.
    assert!(bound::<V2<u8, String>>().is_none());
    assert!(bound::<V2<Vec<u8>, u8>>().is_none());
    assert!(bound::<V2<u8, Free>>().is_none());
};

// ── Tuple: header W, each element its payload, + W when variable ────────────
// (W = 1 for all of these but the one holding Capped.)
const _: () = {
    assert!(matches!(bound::<()>(), Some(1)));
    assert!(matches!(bound::<(u32,)>(), Some(5)));
    assert!(matches!(bound::<(u8, u16, u32, u64)>(), Some(16)));
    assert!(matches!(bound::<(Point, bool)>(), Some(10)));
    assert!(matches!(bound::<(Name,)>(), Some(34)));
    assert!(matches!(bound::<(Option<u32>,)>(), Some(7)));
    assert!(matches!(bound::<((u8,), u8)>(), Some(5)));
    assert!(matches!(bound::<(V2<u8, u32>,)>(), Some(7)));
    // 1 + 300 + 1 + 1 = 303 does not fit one-byte offsets: 2 + 300 + 2 + 1.
    assert!(matches!(bound::<(Capped, u8)>(), Some(305)));
    assert!(matches!(bound::<([u16; 3],)>(), Some(7)));
    // An element has no member annotation: a string or list is unbounded.
    assert!(bound::<(u32, String)>().is_none());
    assert!(bound::<(Vec<u8>,)>().is_none());
    assert!(bound::<(Free,)>().is_none());
    // The 2-tuple (a map's entry included).
    assert!(matches!(bound::<(u32, u8)>(), Some(6)));
    assert!(matches!(bound::<(u32, Name)>(), Some(38)));
    assert!(bound::<(String, u8)>().is_none());
};

// ── Widths: the next power of two that holds the bound ──────────────────────
const _: () = {
    assert!(width::<()>() == 1 && width::<(u32,)>() == 1);
    assert!(width::<(u64, Name, u8)>() == 1);
    assert!(width::<(u32, u8)>() == 1);
    assert!(width::<(Capped, u8)>() == 2); // 305
    assert!(width::<([u64; 40],)>() == 2); // 322
    assert!(width::<([u64; 9000],)>() == 4); // 72004
    // The thresholds: the header counts at the tuple's own width.
    assert!(matches!(bound::<([u8; 254],)>(), Some(255)) && width::<([u8; 254],)>() == 1);
    assert!(matches!(bound::<([u8; 255],)>(), Some(257)) && width::<([u8; 255],)>() == 2);
    assert!(matches!(bound::<([u8; 65533],)>(), Some(65535)) && width::<([u8; 65533],)>() == 2);
    assert!(matches!(bound::<([u8; 65534],)>(), Some(65538)) && width::<([u8; 65534],)>() == 4);
    assert!(width::<(u32, String)>() == 4);
    // A record holding a bounded variant / tuple is bounded itself.
    assert!(matches!(bound::<Event>(), Some(19)) && width::<Event>() == 1);
    assert!(matches!(bound::<Call>(), Some(49)) && width::<Call>() == 1);
    assert!(bound::<Open>().is_none() && width::<Open>() == 4);
    // An unbounded list has a four-byte offset table whatever its elements.
    assert!(
        width::<Vec<(u32,)>>() == 4
            && width::<BTreeMap<u32, u8>>() == 4
            && width::<BTreeSet<(u8,)>>() == 4
    );
};

// ── Bit types: ceil(N / 8) bytes; a bitlist one more for its delimiter ──────
const _: () = {
    assert!(matches!(bound::<Bitvector<1>>(), Some(1)) && matches!(bound::<Bitvector<8>>(), Some(1)));
    assert!(matches!(bound::<Bitvector<9>>(), Some(2)) && matches!(bound::<Bitvector<2040>>(), Some(255)));
    assert!(matches!(bound::<Bitlist<1>>(), Some(2)) && matches!(bound::<Bitlist<8>>(), Some(2)));
    assert!(matches!(bound::<Bitlist<9>>(), Some(3)) && matches!(bound::<Bitlist<12>>(), Some(3)));
    assert!(matches!(bound::<BitFlags>(), Some(8)) && width::<BitFlags>() == 1);
    assert!(matches!(bound::<BitFixed>(), Some(2)));
    // The 255 / 256 threshold: record{bitlist<N>} is 2 * W + ceil(N / 8) + 1.
    assert!(matches!(bound::<BitList2016>(), Some(255)) && width::<BitList2016>() == 1);
    assert!(matches!(bound::<BitList2017>(), Some(258)) && width::<BitList2017>() == 2);
    // In lists and tuples: fixed bitvectors pack with no table; bitlists
    // have one.
    assert!(matches!(bound::<BoundedList<Bitvector<12>, 10>>(), Some(20)));
    assert!(matches!(bound::<BoundedList<Bitlist<12>, 10>>(), Some(40)));
    assert!(width::<BoundedList<Bitlist<12>, 10>>() == 1);
    assert!(matches!(bound::<(Bitvector<12>, Bitlist<12>)>(), Some(7)));
    assert!(matches!(bound::<Option<Bitlist<12>>>(), Some(4)));
};

fn hex(text: &str) -> Vec<u8> {
    (0..text.len()).step_by(2).map(|i| u8::from_str_radix(&text[i..i + 2], 16).unwrap()).collect()
}

/// The typed and the schema-driven codec, on one value.
fn agree<T>(value: T, wire: &str)
where
    T: PsszPack<Pssz> + PsszUnpack<Pssz> + PsszValidate<Pssz> + ToPsszSchema,
    T: PartialEq + std::fmt::Debug,
{
    let wire = hex(wire);
    let bytes = psio::to_pssz(&value);
    assert_eq!(bytes, wire);
    assert_eq!(validate::<Pssz, T>(&bytes), Ok(()));
    assert_eq!(validate_strict::<Pssz, T>(&bytes), Ok(()));
    assert_eq!(psio::from_pssz::<T>(&bytes).unwrap(), value);
    let decoded = decode_checked::<Pssz, T>(&bytes).unwrap();
    assert!(decoded.value == value && !decoded.has_unknown);

    let (schema, root) = schema_for::<T>().unwrap();
    assert_eq!(dynamic::auto_width(&schema, &root).unwrap(), auto_pssz_width::<T>());
    assert_eq!(dynamic::validate(&schema, &root, &bytes), Ok(false));
    let dynamic_value = dynamic::decode_exact::<Pssz>(&schema, &root, &bytes).unwrap();
    assert_eq!(dynamic::encode::<Pssz>(&schema, &root, &dynamic_value).unwrap(), bytes);
}

fn name(text: &str) -> Name {
    BoundedString(text.to_string())
}

#[test]
fn tuples_take_the_width_their_bound_allows() {
    // One-byte header and no slots: all elements fixed.
    agree((), "00");
    agree((9u32,), "0409000000");
    agree((9u32, 7u8), "050900000007");
    // A variable element: one-byte header, one-byte offset slot.
    agree((1u64, name("bob"), 2u8), "0a01000000000000000a02626f62");
    // Trailing optionals prune as in any record.
    agree((1u8, None::<u32>), "0101");
    agree((1u8, Some(5u32)), "02010205000000");
    // Two-byte offsets.
    agree((Capped { text: "hi".into() }, 3u8), "0300030003020002006869");
    // An unbounded element keeps the tuple at four bytes.
    agree((9u32, "hi".to_string()), "0800000009000000080000006869");
}

#[test]
fn a_variant_bounds_the_record_that_holds_it() {
    // The variant itself has no offsets: selector, payload.
    agree(V2::<u8, u32>::B(9), "0109000000");
    agree(V2::<u8, Point>::B(Point { x: 1, y: 2 }), "010100000002000000");
    // Event: one-byte header (9) and slot.
    agree(Event { at: 7, what: V3::A(5) }, "090700000000000000090005");
    agree(
        Event { at: 7, what: V3::B(Point { x: 1, y: 2 }) },
        "09070000000000000009010100000002000000",
    );
    // Call: the argument tuple inside a record, both at one byte.
    agree(
        Call { method: 3, args: (1, name("bob"), 2) },
        "0503000000050a01000000000000000a02626f62",
    );
}

#[test]
fn map_entries_are_bounded_tuples() {
    // The list's offset table stays four bytes wide; each entry is the
    // 2-tuple at its own width (one byte: header 5, u32, u8).
    agree(
        BTreeMap::from([(1u32, 2u8), (3, 4)]),
        "080000000e000000050100000002050300000004",
    );
    agree(vec![(1u32, 2u8)], "04000000050100000002");
}

// ── A tuple has at most 255 elements (docs/pssz-spec.md 8.4) ────────────────
#[test]
fn the_schema_driven_codec_refuses_a_tuple_of_more_than_255_elements() {
    use psio::pssz::schema_ir::{AnyType, AnyValue, Tuple};
    let (schema, byte) = schema_for::<u8>().unwrap();
    let tuple = |count: usize| {
        AnyType::new(AnyValue::Tuple(Tuple { members: (0..count).map(|_| Box::new(byte.clone())).collect() }))
    };
    let value = |count: usize| dynamic::Value::Sequence(vec![dynamic::Value::Unsigned(1); count]);
    assert_eq!(dynamic::MAX_TUPLE_ELEMENTS, 255);
    // 255 elements: 1 + 255 > 255, so a 2-byte header, then the bytes.
    let bytes = dynamic::encode::<Pssz>(&schema, &tuple(255), &value(255)).unwrap();
    assert_eq!(bytes.len(), 2 + 255);
    assert_eq!(dynamic::auto_width(&schema, &tuple(255)), Ok(2));
    assert_eq!(dynamic::validate(&schema, &tuple(255), &bytes), Ok(false));
    // 256: not a type, in any profile and on every entry point.
    assert!(dynamic::encode::<Pssz>(&schema, &tuple(256), &value(256)).is_err());
    assert!(dynamic::encode::<psio::pssz::Pssz32>(&schema, &tuple(256), &value(256)).is_err());
    assert!(dynamic::validate(&schema, &tuple(256), &[0u8; 260]).is_err());
    assert!(dynamic::decode_exact::<Pssz>(&schema, &tuple(256), &[0u8; 260]).is_err());
    assert!(dynamic::auto_width(&schema, &tuple(256)).is_err());
}

// ── Evolution: the widths beneath a cap are part of the layout ──────────────
derives! {
    #[pssz(max_dynamic_data = 255)]
    struct Tags255 { tags: Vec<String> }
    #[pssz(max_dynamic_data = 200)]
    struct Tags200 { tags: Vec<String> }
    #[pssz(max_dynamic_data = 255)]
    struct Tags255More { tags: Vec<String>, more: Option<u8> }
    // Same record width as Wide (4), but the list beneath is 2 wide.
    #[pssz(max_dynamic_data = 70000)]
    struct Wide { inner: Vec<Tags255>, rows: Vec<(u8, Vec<String>)> }
    #[pssz(max_dynamic_data = 70000)]
    struct WideBounded { inner: Vec<Tags255>, rows: psio::pssz::BoundedList<(u8, Vec<String>), 9> }
    struct Opened { child: Tags255, kids: Vec<Free> }
    #[pssz(max_dynamic_data = 255)]
    struct Envelope { child: Free, note: Vec<String> }
    #[pssz(max_dynamic_data = 255)]
    struct Narrow { child: Tags200, kids: Vec<(u8, Tags255)> }
}
const _: () = {
    use psio::pssz::{pssz_child_wider_than_cap, pssz_evolves_from};
    // The same width class: a cap can be raised inside it, not lowered
    // (the new reader would reject old values over the new cap).
    assert!(pssz_evolves_from::<Tags200, Tags255>());
    assert!(!pssz_evolves_from::<Tags255, Tags200>());
    assert!(pssz_evolves_from::<Tags255, Tags255More>());
    // The record under a cap and without one differ in what lies beneath.
    assert!(!pssz_evolves_from::<Free, Tags255>());
    assert!(pssz_evolves_from::<Wide, Wide>());
    assert!(!pssz_evolves_from::<Wide, WideBounded>());
    // A child wider than its capped parent is reported, not refused.
    assert!(pssz_child_wider_than_cap::<Envelope>());
    assert!(!pssz_child_wider_than_cap::<Narrow>());
    assert!(!pssz_child_wider_than_cap::<Opened>() && !pssz_child_wider_than_cap::<Free>());
    assert!(!pssz_child_wider_than_cap::<Vec<Envelope>>());
    // The position is the nearest record's: beneath Tags255 inside Open the
    // list is 1 wide, beside it 4.
    assert!(width::<Vec<String>>() == 4);
    assert!(<Vec<String> as PsszPack<psio::pssz::PsszUnder<255>>>::OFFSET_WIDTH == 1);
    assert!(<Vec<String> as PsszPack<psio::pssz::PsszUnder<256>>>::OFFSET_WIDTH == 2);
    assert!(<(u8, String) as PsszPack<psio::pssz::PsszUnder<255>>>::OFFSET_WIDTH == 1);
    // A record keeps its width at every position.
    assert!(<Free as PsszPack<psio::pssz::PsszUnder<255>>>::OFFSET_WIDTH == 4);
    assert!(<Tags255 as PsszPack<psio::pssz::Pssz32>>::OFFSET_WIDTH == 4);
};

#[test]
fn a_record_has_the_same_bytes_beneath_a_cap_and_at_the_root() {
    let free = Free { text: "hi".into() };
    let envelope = Envelope { child: free.clone(), note: vec!["n".into()] };
    let bytes = psio::to_pssz(&envelope);
    // header 02, slots 02 and 0c; Free with 4-byte offsets; a 1-byte table.
    assert_eq!(bytes, hex("02020c0400000004000000686901" ).iter().copied().chain(*b"n").collect::<Vec<u8>>());
    agree(envelope, "02020c04000000040000006869016e");
    assert_eq!(&bytes[3..13], psio::to_pssz(&free));
}
