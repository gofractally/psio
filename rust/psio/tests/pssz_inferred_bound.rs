// The Rust port of cpp/tests/pssz_inferred_bound_tests.cpp: the inferred
// size bound of variants and tuples (docs/pssz-spec.md 3.2).
//   Variant(Ts...)  1 + max(max_encoded_size(T_i))
//   Tuple(Ts...)    the extensible record of its elements: header + per
//                   element its payload (+ an offset slot when variable)
// with every header and slot counted as 4 bytes. A bounded tuple / variant,
// and a record that holds one, gets one- or two-byte offsets instead of
// four; the typed codec and the schema-driven one agree. A C++ `std::pair`
// is the Rust 2-tuple.

use psio::pssz::schema_ir::{schema_for, ToPsszSchema};
use psio::pssz::{
    auto_pssz_width, decode_checked, dynamic, validate, validate_strict, BoundedString, Pssz,
    PsszPack, PsszUnpack, PsszValidate,
};
use std::collections::{BTreeMap, BTreeSet};

type Name = BoundedString<32>;

const fn bound<T: PsszPack<Pssz>>() -> Option<usize> {
    T::AUTO_BOUND
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

// ── Tuple: header 4, each element its payload, + 4 when variable ────────────
const _: () = {
    assert!(matches!(bound::<()>(), Some(4)));
    assert!(matches!(bound::<(u32,)>(), Some(8)));
    assert!(matches!(bound::<(u8, u16, u32, u64)>(), Some(19)));
    assert!(matches!(bound::<(Point, bool)>(), Some(13)));
    assert!(matches!(bound::<(Name,)>(), Some(40)));
    assert!(matches!(bound::<(Option<u32>,)>(), Some(13)));
    assert!(matches!(bound::<((u8,), u8)>(), Some(14)));
    assert!(matches!(bound::<(V2<u8, u32>,)>(), Some(13)));
    assert!(matches!(bound::<(Capped, u8)>(), Some(309)));
    assert!(matches!(bound::<([u16; 3],)>(), Some(10)));
    // An element has no member annotation: a string or list is unbounded.
    assert!(bound::<(u32, String)>().is_none());
    assert!(bound::<(Vec<u8>,)>().is_none());
    assert!(bound::<(Free,)>().is_none());
    // The 2-tuple (a map's entry included).
    assert!(matches!(bound::<(u32, u8)>(), Some(9)));
    assert!(matches!(bound::<(u32, Name)>(), Some(44)));
    assert!(bound::<(String, u8)>().is_none());
};

// ── Widths: the next power of two that holds the bound ──────────────────────
const _: () = {
    assert!(width::<()>() == 1 && width::<(u32,)>() == 1);
    assert!(width::<(u64, Name, u8)>() == 1);
    assert!(width::<(u32, u8)>() == 1);
    assert!(width::<(Capped, u8)>() == 2); // 309
    assert!(width::<([u64; 40],)>() == 2); // 324
    assert!(width::<([u64; 9000],)>() == 4); // 72004
    assert!(width::<(u32, String)>() == 4);
    // A record holding a bounded variant / tuple is bounded itself.
    assert!(matches!(bound::<Event>(), Some(25)) && width::<Event>() == 1);
    assert!(matches!(bound::<Call>(), Some(61)) && width::<Call>() == 1);
    assert!(bound::<Open>().is_none() && width::<Open>() == 4);
    // Lists keep four-byte offset tables whatever their elements.
    assert!(
        width::<Vec<(u32,)>>() == 4
            && width::<BTreeMap<u32, u8>>() == 4
            && width::<BTreeSet<(u8,)>>() == 4
    );
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
