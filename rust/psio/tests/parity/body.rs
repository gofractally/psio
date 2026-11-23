// Included by ../pssz_parity.rs, which defines `rec!` / `dwnc!` (a record
// declaration with the PSSZ derives). Type names mirror
// cpp/tests/pssz_evolution_fixture.hpp and pssz_type_width_fixture.hpp.
// C++ std::string is RawString; a length_bound member is a Bounded* type.

use super::support::*;
use psio::pssz::*;
use psio::pssz_view::{PsszView};

// ── Evolution lineage (pssz_evolution_fixture.hpp) ──────────────────────────
rec!(R0 { a: u32, s: RawString });
rec!(R1 { a: u32, s: RawString, x: Option<u32> });
rec!(R2 { a: u32, s: RawString, x: Option<u32>, y: Option<RawString> });
rec!(R3 { a: u32, s: RawString, x: Option<u32>, y: Option<RawString>, z: Option<Vec<u16>> });
rec!(Inner { k: u8, t: RawString });
rec!(N1 { a: u32, s: RawString, inner: Option<Inner> });
rec!(P1 { a: u32, s: RawString, oo: Option<Option<u32>> });
rec!(Bad1 { a: u32, s: RawString, b: u32 });
rec!(Parent0 { child: R0, tail: RawString });
rec!(Parent2 { child: R2, tail: RawString });

#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack)]
pub enum VarNew { Number(u32), Record(R2) }
#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack)]
pub enum VarOld { Number(u32), Record(R0) }
#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack)]
pub enum VarR1 { Number(u32), Record(R1) }

// ── Type-width fixture (pssz_type_width_fixture.hpp) ────────────────────────
rec!(#[cap(64)] WidthCheckpoint { height: u32, label: BoundedRawString<16> });
#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack)]
pub enum Proof { Checkpoint(WidthCheckpoint), Number(u32) }
rec!(WidthRelayBatch {
    checkpoint: WidthCheckpoint,
    entries: Vec<WidthCheckpoint>,
    latest: Option<WidthCheckpoint>,
    pair: [WidthCheckpoint; 2],
    proof: Proof,
    context: RawString,
});
rec!(#[cap(1000000)] WidthLargeBudget { value: u8 });
rec!(#[cap(64)] WidthNarrowEnvelope { child: WidthLargeBudget, sequence: u32 });
rec!(WidthReserveParent { child: WidthLargeBudget });
rec!(#[cap(512)] WidthGrowV1 { id: u32 });
rec!(#[cap(512)] WidthGrowV2 { id: u32, time: Option<u32> });
rec!(WidthGrowParentV1 { child: WidthGrowV1 });
rec!(WidthGrowParentV2 { child: WidthGrowV2 });
rec!(WidthThreshold251 { value: [u8; 251] });
rec!(WidthThreshold252 { value: [u8; 252] });
rec!(WidthThreshold253 { value: [u8; 253] });
rec!(WidthGrown253 { value: [u8; 253], more: Option<u8> });
rec!(WidthBoundedBatch { entries: BoundedList<WidthCheckpoint, 4> });
rec!(WidthStringPair { first: RawString, second: RawString });
rec!(#[cap(255)] WidthCappedPair { first: RawString, second: RawString });
rec!(#[cap(65535)] WidthMediumPair { first: RawString, second: RawString });
rec!(WidthOnlyTail { payload: RawString });
dwnc!(#[cap(4)] WidthBadFixed { value: u64 });
rec!(WidthBadFixedParent { fixed: WidthBadFixed, tail: RawString });
rec!(WidthCappedParent { pair: WidthCappedPair, tail: RawString });
rec!(#[cap(200)] WidthNestedContainers {
    nested: Vec<Vec<WidthCheckpoint>>,
    optional: Vec<Option<WidthCheckpoint>>,
});

// A bounded UTF-8 string feeds width inference; an unbounded one does not.
rec!(Label16 { id: u32, label: BoundedString<16> });
rec!(Label300 { id: u32, label: BoundedString<300> });
rec!(LabelAny { id: u32, label: String });
rec!(LabelMaybe { id: u32, label: Option<BoundedString<16>> });

// ── Presence classes, zero-size and fixed-layout records ────────────────────
dwnc!(Unit {});
rec!(Empty {});
dwnc!(Pair { x: u8, y: u8 });
#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
#[psio(definition_will_not_change)]
pub struct DwncVar { pub a: u8, pub s: RawString, pub b: u16 }
rec!(Middle { a: u32, inner: Option<Inner>, s: RawString });
rec!(Tail { a: u32, s: RawString, unit: Option<Unit>, empty: Option<Empty>, pair: Option<Pair> });

// ── Enums ───────────────────────────────────────────────────────────────────
#[derive(Debug, Clone, Copy, PartialEq, psio::PsszPack, psio::PsszUnpack)]
#[repr(u8)]
pub enum Color { Red = 0, Green = 1, Blue = 7 }
#[derive(Debug, Clone, Copy, PartialEq, psio::PsszPack, psio::PsszUnpack)]
#[repr(u16)]
pub enum Wide { A = 1, B = 0x1234 }
#[derive(Debug, Clone, Copy, PartialEq, psio::PsszPack, psio::PsszUnpack)]
#[repr(i32)]
pub enum Signed { Minus = -2, Plus = 5 }
#[derive(Debug, Clone, Copy, PartialEq, psio::PsszPack, psio::PsszUnpack)]
#[repr(u64)]
pub enum Big { Low = 3, High = 0x0102_0304_0506_0708 }
rec!(Painted { color: Color, name: RawString, wide: Option<Wide> });
#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack)]
pub enum Shape3 { Nothing, Text(String), Point(u8, RawString), Named { a: u16, b: Option<u8> } }

// ── Strings ─────────────────────────────────────────────────────────────────
rec!(Texts { utf8: String, rawstr: RawString, bounded: BoundedString<4>, raw_bounded: BoundedRawString<4> });

fn raw(s: &str) -> RawString { RawString::from(s) }
fn r0() -> R0 { R0 { a: 7, s: raw("hi") } }
fn column<F: PsszFormat>() -> usize {
    // The automatic profile selects four-byte offsets for every type in the
    // evolution corpus and must equal the pssz32 column.
    if F::AUTO { 2 } else { match F::OFFSET_BYTES { 1 => 0, 2 => 1, _ => 2 } }
}
fn width<F: PsszFormat>() -> usize { if F::AUTO { 4 } else { F::OFFSET_BYTES } }
/// The width of `T`'s own header and slots: under the automatic profile a
/// bounded tuple has narrower ones than an unbounded type.
fn width_of<F: PsszFormat, T: PsszPack<F>>() -> usize { T::OFFSET_WIDTH }

/// C++ `reads<F, Reader>`: every entry point accepts and agrees.
fn reads<F, T>(wire: &[u8], expect: &T, unknown: bool)
where
    F: PsszFormat,
    T: PsszPack<F> + PsszUnpack<F> + PsszValidate<F> + PartialEq + std::fmt::Debug,
{
    assert_eq!(validate::<F, T>(wire), Ok(()));
    assert_eq!(validate_compatible::<F, T>(wire), Ok(unknown));
    assert_eq!(&decode::<F, T>(wire).unwrap(), expect);
    let decoded = decode_checked::<F, T>(wire).unwrap();
    assert_eq!(decoded.has_unknown, unknown);
    assert_eq!(&decoded.value, expect);
    assert_eq!(validate_strict::<F, T>(wire).is_ok(), !unknown);
    assert_eq!(decode_strict::<F, T>(wire).is_ok(), !unknown);
    assert_eq!(PsszView::<T, F>::new(wire).is_ok(), true);
    assert_eq!(PsszView::<T, F>::new_checked(wire).unwrap().has_unknown, unknown);
    if !unknown {
        assert_eq!(try_encode::<F, T>(expect).unwrap(), wire);
        assert_eq!(validate_canonical_for_profile::<F, T>(wire), Ok(()));
    }
}
/// C++ `rejects<F, Reader>`: every entry point rejects, none panics.
fn rejects<F, T>(wire: &[u8])
where
    F: PsszFormat,
    T: PsszPack<F> + PsszUnpack<F> + PsszValidate<F> + std::fmt::Debug,
{
    assert!(validate::<F, T>(wire).is_err(), "validate accepted {wire:02x?}");
    assert!(validate_compatible::<F, T>(wire).is_err());
    assert!(validate_strict::<F, T>(wire).is_err());
    assert!(decode::<F, T>(wire).is_err(), "decode accepted {wire:02x?}");
    assert!(decode_checked::<F, T>(wire).is_err());
    assert!(decode_strict::<F, T>(wire).is_err());
    assert!(PsszView::<T, F>::new(wire).is_err());
}

macro_rules! each_format {
    ($name:ident, $body:ident) => {
        #[test]
        fn $name() {
            $body::<Pssz8>();
            $body::<Pssz16>();
            $body::<Pssz32>();
            $body::<Pssz>();
        }
    };
}

// ── Rule 5: type-derived widths ─────────────────────────────────────────────

#[test]
fn automatic_widths_and_bounds_match_the_cpp_static_assertions() {
    fn bound<T: PsszPack<Pssz>>() -> Option<usize> { T::AUTO_BOUND }
    // pssz_type_width_tests.cpp STATIC_REQUIREs.
    assert_eq!(auto_pssz_width::<WidthCheckpoint>(), 1);
    assert_eq!(auto_pssz_width::<WidthRelayBatch>(), 4);
    assert_eq!(auto_pssz_width::<WidthNestedContainers>(), 1);
    assert_eq!(auto_pssz_width::<Vec<WidthCheckpoint>>(), 4);
    assert_eq!(auto_pssz_width::<WidthLargeBudget>(), 4);
    assert_eq!(auto_pssz_width::<WidthNarrowEnvelope>(), 1);
    assert_eq!(auto_pssz_width::<WidthReserveParent>(), 4);
    assert_eq!(bound::<WidthReserveParent>(), Some(1_000_008));
    assert_eq!(auto_pssz_width::<WidthGrowV1>(), 2);
    assert_eq!(auto_pssz_width::<WidthGrowV2>(), 2);
    assert_eq!(auto_pssz_width::<WidthGrowParentV1>(), 2);
    assert_eq!(auto_pssz_width::<WidthGrowParentV2>(), 2);
    assert_eq!(bound::<WidthGrowParentV1>(), bound::<WidthGrowParentV2>());
    // The header and slots count at the type's own width (pssz-spec 3.2
    // fit): 1 + 251, 1 + 252, 1 + 253.
    assert_eq!(bound::<WidthThreshold251>(), Some(252));
    assert_eq!(bound::<WidthThreshold252>(), Some(253));
    assert_eq!(bound::<WidthThreshold253>(), Some(254));
    assert_eq!(auto_pssz_width::<WidthThreshold251>(), 1);
    assert_eq!(auto_pssz_width::<WidthThreshold252>(), 1);
    // Four checkpoints of at most 64 bytes: 4 * (1 + 64) = 260 does not fit
    // a one-byte table, 4 * (2 + 64) = 264 does a two-byte one. The record
    // that holds the list adds its own header and slot: 2 + 2 + 264.
    assert_eq!(bound::<WidthBoundedBatch>(), Some(268));
    assert_eq!(auto_pssz_width::<WidthBoundedBatch>(), 2);
    assert_eq!(bound::<[WidthCheckpoint; 4]>(), Some(264));
    assert_eq!(auto_pssz_width::<[WidthCheckpoint; 4]>(), 2);
    assert_eq!(auto_pssz_width::<WidthMediumPair>(), 2);
    assert_eq!(auto_pssz_width::<WidthCappedPair>(), 1);
    assert_eq!(auto_pssz_width::<WidthStringPair>(), 4);
    // cpp/tests/pssz_string_tests.cpp: (header + slot) x W + u32 4 + N.
    assert_eq!(bound::<BoundedString<16>>(), Some(16));
    assert_eq!(bound::<Label16>(), Some(22));
    assert_eq!(auto_pssz_width::<Label16>(), 1);
    assert_eq!(bound::<Label300>(), Some(308));
    assert_eq!(auto_pssz_width::<Label300>(), 2);
    assert_eq!(bound::<LabelAny>(), None);
    assert_eq!(auto_pssz_width::<LabelAny>(), 4);
    assert_eq!(bound::<LabelMaybe>(), Some(23));
    assert_eq!(bound::<String>(), None);
    assert_eq!(bound::<RawString>(), None);
    assert_eq!(bound::<BoundedRawString<16>>(), Some(16));
    assert_eq!(
        try_to_pssz(&Label16 { id: 7, label: BoundedString("hi".into()) }).unwrap(),
        hex("0507000000056869")
    );
    // Every evolution-corpus type holds an unbounded string: width 4.
    assert_eq!(auto_pssz_width::<R0>(), 4);
    assert_eq!(auto_pssz_width::<R3>(), 4);
    assert_eq!(auto_pssz_width::<Parent2>(), 4);
    // Explicit tags ignore the type: one inherited width.
    assert_eq!(<WidthCheckpoint as PsszPack<Pssz32>>::OFFSET_WIDTH, 4);
    assert_eq!(<WidthRelayBatch as PsszPack<Pssz8>>::OFFSET_WIDTH, 1);
}

fn sample() -> WidthRelayBatch {
    let cp = |height, label: &str| WidthCheckpoint { height, label: BoundedRawString(label.as_bytes().to_vec()) };
    WidthRelayBatch {
        checkpoint: cp(7, "main"),
        entries: vec![cp(8, "one"), cp(9, "two")],
        latest: Some(cp(10, "last")),
        pair: [cp(11, "left"), cp(12, "right")],
        proof: Proof::Checkpoint(cp(13, "proof")),
        context: raw("relay"),
    }
}

#[test]
fn explicit_profiles_reproduce_the_cpp_reference_bytes() {
    fn check<F: PsszFormat>() {
        let goldens = width_goldens();
        let golden = |name: &str| goldens[&format!("{name}-{}", F::OFFSET_BYTES)].clone();
        let value = sample();
        reads::<F, _>(&golden("relay"), &value, false);
        reads::<F, _>(&golden("vector"), &value.entries, false);
        reads::<F, _>(&golden("array"), &value.pair, false);
        reads::<F, _>(&golden("some"), &value.latest, false);
        reads::<F, Option<WidthCheckpoint>>(&golden("none"), &None, false);
        reads::<F, _>(&golden("variant-record"), &value.proof, false);
        reads::<F, _>(&golden("variant-number"), &Proof::Number(42), false);
        reads::<F, _>(
            &golden("narrow-parent"),
            &WidthNarrowEnvelope { child: WidthLargeBudget { value: 42 }, sequence: 17 },
            false,
        );
    }
    check::<Pssz8>();
    check::<Pssz16>();
    check::<Pssz32>();
}

#[test]
fn automatic_profile_keeps_each_type_at_its_own_width() {
    let goldens = width_goldens();
    let value = sample();
    // A width-1 child keeps its width-1 bytes inside the width-4 parent, a
    // vector of them, an optional, an array and a variant.
    let child = try_to_pssz(&value.checkpoint).unwrap();
    assert_eq!(child, hex("0507000000056d61696e"));
    assert_eq!(child, try_encode::<Pssz8, _>(&value.checkpoint).unwrap());
    let wire = try_to_pssz(&value).unwrap();
    reads::<Pssz, _>(&wire, &value, false);
    // Parent frame: 4-byte header and slots; each record payload is the
    // width-1 encoding; the vector table is 4 bytes wide, the array's is 2.
    let entries = try_to_pssz(&value.entries).unwrap();
    assert_eq!(&entries[..8], hex("0800000011000000"));
    assert_eq!(&entries[8..17], hex("0508000000056f6e65"));
    let pair = try_to_pssz(&value.pair).unwrap();
    // [WidthCheckpoint; 2] is bounded by 2 * (64 + 4) = 136: one-byte table.
    assert_eq!(auto_pssz_width::<[WidthCheckpoint; 2]>(), 1);
    assert_eq!(pair, goldens["array-1"]);
    assert_eq!(try_to_pssz(&value.latest).unwrap(), goldens["some-1"]);
    assert_eq!(try_to_pssz(&value.proof).unwrap(), goldens["variant-record-1"]);
    assert_ne!(wire, goldens["relay-4"]);
    let at = |i: usize| u32::from_le_bytes(wire[4 + 4 * i..8 + 4 * i].try_into().unwrap()) as usize + 4;
    assert_eq!(&wire[at(0)..at(1)], child);
    assert_eq!(&wire[at(1)..at(2)], entries);
    assert_eq!(&wire[at(3)..at(4)], pair);

    // A width-4 child (large declared budget) inside a width-1 parent.
    let narrow = WidthNarrowEnvelope { child: WidthLargeBudget { value: 42 }, sequence: 17 };
    let wire = try_to_pssz(&narrow).unwrap();
    assert_eq!(wire, hex("050511000000010000002a"));
    reads::<Pssz, _>(&wire, &narrow, false);
    // The same value under the explicit tags inherits one width.
    assert_eq!(try_encode::<Pssz8, _>(&narrow).unwrap(), goldens["narrow-parent-1"]);

    // Nested containers: the record is width 1 (cap 200), and so are the
    // tables of the lists beneath its cap (docs/pssz-spec.md 3.6); a
    // checkpoint is a record and keeps its own width.
    let cp = |height, label: &str| WidthCheckpoint { height, label: BoundedRawString(label.as_bytes().to_vec()) };
    let nested = WidthNestedContainers {
        nested: vec![vec![cp(1, "a")], vec![cp(2, "b")]],
        optional: vec![Some(cp(3, "c")), None],
    };
    let wire = try_to_pssz(&nested).unwrap();
    assert_eq!(&wire[..3], [2, 2, 2 + 2 + 2 * (1 + 7)]);
    assert_eq!(&wire[3..21], hex("020a01050100000005610105020000000562"));
    reads::<Pssz, _>(&wire, &nested, false);
}

#[test]
fn caps_and_offset_widths_reject_overflow_without_widening() {
    let big = RawString(vec![b'x'; 260]);
    let oversized = WidthCappedPair { first: big.clone(), second: raw("last") };
    assert!(try_to_pssz(&oversized).is_err());
    let mut destination = b"pre".to_vec();
    assert!(<WidthCappedPair as PsszPack<Pssz>>::pssz_pack_checked(&oversized, &mut destination).is_err());
    assert_eq!(destination, b"pre");
    // Explicit narrow tag: the stored offset does not fit.
    let explicit = WidthStringPair { first: big.clone(), second: raw("last") };
    assert!(try_encode::<Pssz8, _>(&explicit).is_err());
    let wide = try_encode::<Pssz16, _>(&explicit).unwrap();
    reads::<Pssz16, _>(&wide, &explicit, false);
    // The cap is a property of the type: enforced when nested (automatic
    // profile enforces it at every node; explicit tags at the top level only,
    // as C++ does).
    let parent = WidthCappedParent { pair: oversized.clone(), tail: raw("t") };
    assert!(try_to_pssz(&parent).is_err());
    let explicit_parent = try_encode::<Pssz32, _>(&parent).unwrap();
    assert!(decode::<Pssz32, WidthCappedParent>(&explicit_parent).is_ok());
    assert!(try_encode::<Pssz32, _>(&oversized).is_err());
    // Readers reject a frame over its cap, top level and nested.
    let child16 = try_encode::<Pssz16, _>(&explicit).unwrap();
    rejects::<Pssz16, WidthCappedPair>(&child16);
    let mut over = Frame::new(1).o(2).o(2).o(2).bytes;
    over.extend(std::iter::repeat(b'x').take(253));
    assert_eq!(over.len(), 256);
    rejects::<Pssz, WidthCappedPair>(&over);
    let mut nested = Frame::new(4).o(8).o(8).o(8 + 256).bytes;
    nested.extend_from_slice(&over);
    nested.push(b't');
    rejects::<Pssz, WidthCappedParent>(&nested);
    over.pop();
    assert!(from_pssz::<WidthCappedPair>(&over).is_ok());

    // 65535 cap: two-byte offsets, 65536 bytes do not fit.
    let medium = WidthMediumPair { first: RawString(vec![b'x'; 65536]), second: raw("tail") };
    assert!(try_to_pssz(&medium).is_err());
    // A 256-slot table does not fit one-byte offsets.
    assert!(try_encode::<Pssz8, _>(&vec![RawString::new(); 256]).is_err());
    // Explicit profiles constrain stored offsets, not an unstored final end.
    let tail = WidthOnlyTail { payload: RawString(vec![b'x'; 512]) };
    let wire = try_encode::<Pssz8, _>(&tail).unwrap();
    reads::<Pssz8, _>(&wire, &tail, false);

    // A fixed payload larger than its declared budget can never be encoded
    // or accepted in the automatic profile; an empty vector of it is fine.
    let bad = WidthBadFixedParent { fixed: WidthBadFixed { value: 1 }, tail: raw("tail") };
    assert!(try_to_pssz(&bad).is_err());
    assert!(try_to_pssz(&vec![WidthBadFixed { value: 1 }]).is_err());
    rejects::<Pssz, Vec<WidthBadFixed>>(&[0; 8]);
    reads::<Pssz, Vec<WidthBadFixed>>(&[], &vec![], false);
}

// ── Rules 1 and 2: evolution and mandatory pruning ──────────────────────────

fn evolution_corpus_matches_cpp_goldens<F: PsszFormat>() {
    let goldens = evolution_goldens();
    let g = |name: &str| goldens[name][column::<F>()].clone();
    let some = |v: &str| Some(raw(v));
    let r2 = |a, s: &str, x, y| R2 { a, s: raw(s), x, y };
    reads::<F, _>(&g("r0"), &r0(), false);
    reads::<F, _>(&g("r1_none"), &R1 { a: 7, s: raw("hi"), x: None }, false);
    reads::<F, _>(&g("r1_some"), &R1 { a: 7, s: raw("hi"), x: Some(9) }, false);
    reads::<F, _>(&g("r2_none_none"), &r2(7, "hi", None, None), false);
    reads::<F, _>(&g("r2_some_none"), &r2(7, "hi", Some(9), None), false);
    reads::<F, _>(&g("r2_none_some"), &r2(7, "hi", None, some("yo")), false);
    reads::<F, _>(&g("r2_none_empty"), &r2(7, "hi", None, some("")), false);
    let r3 = |x, y, z| R3 { a: 7, s: raw("hi"), x, y, z };
    reads::<F, _>(&g("r3_none"), &r3(None, None, None), false);
    reads::<F, _>(&g("r3_all"), &r3(Some(9), some("yo"), Some(vec![1, 2])), false);
    reads::<F, _>(&g("r3_mid"), &r3(None, some("yo"), None), false);
    reads::<F, _>(&g("r3_empty_vec"), &r3(None, None, Some(vec![])), false);
    reads::<F, _>(&g("n1_none"), &N1 { a: 7, s: raw("hi"), inner: None }, false);
    reads::<F, _>(&g("n1_some"), &N1 { a: 7, s: raw("hi"), inner: Some(Inner { k: 3, t: raw("t") }) }, false);
    reads::<F, _>(&g("p1_none"), &P1 { a: 7, s: raw("hi"), oo: None }, false);
    reads::<F, _>(&g("p1_some_none"), &P1 { a: 7, s: raw("hi"), oo: Some(None) }, false);
    reads::<F, _>(&g("p1_some_some"), &P1 { a: 7, s: raw("hi"), oo: Some(Some(5)) }, false);
    let new = r2(1, "a", Some(9), some("b"));
    reads::<F, _>(&g("vec_new"), &vec![new.clone(), r2(2, "c", None, None)], false);
    reads::<F, _>(&g("opt_new"), &Some(new.clone()), false);
    reads::<F, _>(&g("var_new"), &VarNew::Record(new.clone()), false);
    reads::<F, _>(&g("parent_new"), &Parent2 { child: new.clone(), tail: raw("tail") }, false);

    // Pruning identities: an all-unset suffix is the first revision's bytes.
    for name in ["r1_none", "r2_none_none", "r3_none", "n1_none", "p1_none"] {
        assert_eq!(g(name), g("r0"), "{name}");
    }
    assert_eq!(g("r2_some_none"), g("r1_some"));
    assert_eq!(g("r3_mid"), g("r2_none_some"));
    // Present-but-empty is a value, not a default.
    assert_ne!(g("r2_none_empty"), g("r0"));
    assert_ne!(g("r3_empty_vec"), g("r0"));
    assert_ne!(g("p1_some_none"), g("r0"));

    // Old data → newer readers: missing trailing optionals are None.
    reads::<F, _>(&g("r0"), &R1 { a: 7, s: raw("hi"), x: None }, false);
    reads::<F, _>(&g("r0"), &r2(7, "hi", None, None), false);
    reads::<F, _>(&g("r0"), &r3(None, None, None), false);
    reads::<F, _>(&g("r0"), &N1 { a: 7, s: raw("hi"), inner: None }, false);
    reads::<F, _>(&g("r0"), &P1 { a: 7, s: raw("hi"), oo: None }, false);
    reads::<F, _>(&g("r1_some"), &r2(7, "hi", Some(9), None), false);
    reads::<F, _>(&g("r1_some"), &r3(Some(9), None, None), false);
    reads::<F, _>(&g("r2_none_some"), &r3(None, some("yo"), None), false);
    reads::<F, _>(&g("r2_none_empty"), &r3(None, some(""), None), false);
    let old_vec = try_encode::<F, _>(&vec![R0 { a: 1, s: raw("a") }, R0 { a: 2, s: raw("c") }]).unwrap();
    reads::<F, _>(&old_vec, &vec![r2(1, "a", None, None), r2(2, "c", None, None)], false);
    let old_opt = try_encode::<F, _>(&Some(R0 { a: 1, s: raw("a") })).unwrap();
    reads::<F, _>(&old_opt, &Some(r2(1, "a", None, None)), false);
    reads::<F, Option<R2>>(&try_encode::<F, Option<R0>>(&None).unwrap(), &None, false);
    let old_var = try_encode::<F, _>(&VarOld::Record(R0 { a: 1, s: raw("a") })).unwrap();
    reads::<F, _>(&old_var, &VarNew::Record(r2(1, "a", None, None)), false);
    reads::<F, _>(&try_encode::<F, _>(&VarOld::Number(5)).unwrap(), &VarNew::Number(5), false);
    let old_parent = try_encode::<F, _>(&Parent0 { child: R0 { a: 1, s: raw("a") }, tail: raw("tail") }).unwrap();
    reads::<F, _>(&old_parent, &Parent2 { child: r2(1, "a", None, None), tail: raw("tail") }, false);

    // New data → older readers: unknown slots are skipped but still delimit
    // `s`, and are reported.
    reads::<F, _>(&g("r1_some"), &r0(), true);
    reads::<F, _>(&g("r1_none"), &r0(), false);
    reads::<F, _>(&g("r2_none_some"), &r0(), true);
    reads::<F, _>(&g("r2_none_some"), &R1 { a: 7, s: raw("hi"), x: None }, true);
    reads::<F, _>(&g("r2_some_none"), &R1 { a: 7, s: raw("hi"), x: Some(9) }, false);
    reads::<F, _>(&g("r3_all"), &r0(), true);
    reads::<F, _>(&g("r3_all"), &R1 { a: 7, s: raw("hi"), x: Some(9) }, true);
    reads::<F, _>(&g("r3_all"), &r2(7, "hi", Some(9), some("yo")), true);
    reads::<F, _>(&g("r3_empty_vec"), &r2(7, "hi", None, None), true);
    reads::<F, _>(&g("n1_some"), &r0(), true);
    reads::<F, _>(&g("p1_some_none"), &r0(), true);
    reads::<F, _>(&g("p1_some_some"), &r0(), true);
    reads::<F, _>(&g("vec_new"), &vec![R0 { a: 1, s: raw("a") }, R0 { a: 2, s: raw("c") }], true);
    reads::<F, _>(&g("opt_new"), &Some(R0 { a: 1, s: raw("a") }), true);
    reads::<F, _>(&g("var_new"), &VarOld::Record(R0 { a: 1, s: raw("a") }), true);
    reads::<F, _>(&g("parent_new"), &Parent0 { child: R0 { a: 1, s: raw("a") }, tail: raw("tail") }, true);
    let unset = try_encode::<F, _>(&vec![r2(2, "c", None, None)]).unwrap();
    reads::<F, _>(&unset, &vec![R0 { a: 2, s: raw("c") }], false);
}
each_format!(evolution_corpus_matches_cpp_goldens_in_every_profile, evolution_corpus_matches_cpp_goldens);

fn unpruned_and_malformed_frames_are_rejected<F: PsszFormat>() {
    let goldens = evolution_goldens();
    let g = |name: &str| goldens[name][column::<F>()].clone();
    let w = width::<F>();
    let (k0, k1, k2) = (4 + w, 4 + 2 * w, 4 + 3 * w);
    let f = || Frame::new(w);

    // A stored trailing None is non-canonical for every reader that knows it.
    let fixed_none = f().o(k1).u32(7).o(k1).o(k1 + 2).raw("hi").bytes;
    rejects::<F, R1>(&fixed_none);
    rejects::<F, R2>(&fixed_none);
    rejects::<F, R3>(&fixed_none);
    let selector_none = f().o(k2).u32(7).o(k2).o(k2 + 2).o(k2 + 6).raw("hi").u32(9).byte(0).bytes;
    rejects::<F, R2>(&selector_none);
    rejects::<F, R3>(&selector_none);
    let all_none = f().o(k2).u32(7).o(k2).o(k2 + 2).o(k2 + 2).raw("hi").byte(0).bytes;
    rejects::<F, R2>(&all_none);
    rejects::<F, R3>(&all_none);
    assert_eq!(
        decode::<F, R1>(&fixed_none).unwrap_err(),
        PsszError::new("pssz: trailing absent optional field was not pruned")
    );
    // An older reader cannot judge the writer's last slot: accepts, reports.
    reads::<F, _>(&fixed_none, &r0(), true);
    reads::<F, _>(&selector_none, &R1 { a: 7, s: raw("hi"), x: Some(9) }, true);

    // Unpruned records are rejected wherever they are nested.
    let unpruned = fixed_none.clone();
    let mut in_vector = f().o(w).bytes;
    in_vector.extend_from_slice(&unpruned);
    rejects::<F, Vec<R1>>(&in_vector);
    rejects::<F, Option<R1>>(&unpruned);
    let mut in_variant = vec![1u8];
    in_variant.extend_from_slice(&unpruned);
    rejects::<F, VarR1>(&in_variant);
    let mut in_tuple = f().o(w).o(w).bytes;
    in_tuple.extend_from_slice(&unpruned);
    rejects::<F, (R1,)>(&in_tuple);
    let mut ok_vector = f().o(w).bytes;
    ok_vector.extend_from_slice(&g("r0"));
    reads::<F, _>(&ok_vector, &vec![R1 { a: 7, s: raw("hi"), x: None }], false);

    // The header must cover the non-optional prefix on a field boundary.
    rejects::<F, R0>(&f().o(4).u32(7).bytes);
    rejects::<F, R1>(&f().o(4).u32(7).bytes);
    rejects::<F, R0>(&f().o(0).bytes);
    rejects::<F, R0>(&f().o(2).byte(7).byte(0).bytes);
    rejects::<F, R3>(&f().o(2).byte(7).byte(0).bytes);
    rejects::<F, R0>(&[]);
    if w > 1 {
        rejects::<F, R0>(&[5]);
        let partial = f().o(k0 + 1).u32(7).o(k0 + 1).byte(0).raw("hi").bytes;
        rejects::<F, R0>(&partial);
        rejects::<F, R1>(&partial);
        rejects::<F, R2>(&partial);
    }
    // The header and offsets must stay inside the frame.
    rejects::<F, R0>(&f().o(200).u32(7).o(200).raw("hi").bytes);
    rejects::<F, R1>(&f().o(200).u32(7).o(200).raw("hi").bytes);
    rejects::<F, R0>(&f().o(k0 + w).u32(7).o(k0 + w).bytes);
    rejects::<F, R0>(&f().o(k0).u32(7).o(k0 + 1).raw("xhi").bytes);
    rejects::<F, R0>(&f().o(k0).u32(7).o(k0 - 1).raw("hi").bytes);
    rejects::<F, R0>(&f().o(k0).u32(7).o(k0 + 9).raw("hi").bytes);
    rejects::<F, R0>(&f().o(k1).u32(7).o(k1 + 1).o(k1).raw("hi").bytes);
    rejects::<F, R1>(&f().o(k1).u32(7).o(k1 + 1).o(k1).raw("hi").bytes);
    rejects::<F, R0>(&f().o(k1).u32(7).o(k1).o(k1 + 3).raw("hi").bytes);
    // An appended non-optional field is not a compatible revision.
    rejects::<F, Bad1>(&g("r0"));
    if w < 4 {
        rejects::<F, Bad1>(&g("r1_some"));
    }
    let bad = Bad1 { a: 7, s: raw("hi"), b: 9 };
    reads::<F, _>(&try_encode::<F, _>(&bad).unwrap(), &bad, false);

    // Truncations and single-byte corruptions: never a panic; whatever is
    // accepted without unknown fields is canonical.
    for name in ["r3_all", "n1_some", "p1_some_some", "r2_none_empty"] {
        let bytes = g(name);
        let mut cases: Vec<Vec<u8>> = (0..bytes.len()).map(|n| bytes[..n].to_vec()).collect();
        for i in 0..bytes.len() {
            let mut mutated = bytes.clone();
            mutated[i] ^= 0x5a;
            cases.push(mutated);
        }
        for case in cases {
            let valid = validate_compatible::<F, R3>(&case);
            let decoded = decode::<F, R3>(&case);
            assert_eq!(valid.is_ok(), decoded.is_ok(), "{case:02x?}");
            if valid == Ok(false) {
                assert_eq!(try_encode::<F, _>(&decoded.unwrap()).unwrap(), case);
            }
        }
    }
}
each_format!(unpruned_and_malformed_frames_are_rejected_in_every_profile, unpruned_and_malformed_frames_are_rejected);

fn views_read_evolved_records<F: PsszFormat>() {
    let goldens = evolution_goldens();
    let g = |name: &str| goldens[name][column::<F>()].clone();
    let newest = g("r3_all");
    let old = PsszView::<R0, F>::new(&newest).unwrap();
    assert_eq!(old.a().get(), 7);
    assert_eq!(old.s().bytes(), b"hi");
    let mid = PsszView::<R1, F>::new(&newest).unwrap();
    assert_eq!(mid.s().bytes(), b"hi");
    assert_eq!(mid.x().get().unwrap().get(), 9);
    let same = PsszView::<R3, F>::new(&newest).unwrap();
    assert_eq!(same.y().unwrap().bytes(), b"yo");
    assert_eq!(same.z().unwrap().len(), 2);
    assert_eq!(same.z().unwrap().get(1).get(), 2);
    // Old data, new reader: pruned fields read as absent.
    let oldest = g("r0");
    let upgraded = PsszView::<R3, F>::new(&oldest).unwrap();
    assert_eq!(upgraded.s().bytes(), b"hi");
    assert!(!upgraded.x().has_value());
    assert!(!upgraded.y().has_value());
    assert!(!upgraded.z().has_value());
    assert!(!PsszView::<N1, F>::new(&oldest).unwrap().inner().has_value());
    let partial_bytes = g("r2_none_some");
    let partial = PsszView::<R3, F>::new(&partial_bytes).unwrap();
    assert!(!partial.x().has_value());
    assert_eq!(partial.y().unwrap().bytes(), b"yo");
    assert!(!partial.z().has_value());
    let empty_bytes = g("r2_none_empty");
    let present_empty = PsszView::<R2, F>::new(&empty_bytes).unwrap();
    assert!(present_empty.y().has_value());
    assert!(present_empty.y().unwrap().is_empty());
    let nested_bytes = g("n1_some");
    let nested = PsszView::<N1, F>::new(&nested_bytes).unwrap();
    assert_eq!(nested.inner().unwrap().k().get(), 3);
    assert_eq!(nested.inner().unwrap().t().bytes(), b"t");
    let vec_bytes = g("vec_new");
    let elements = PsszView::<Vec<R0>, F>::new(&vec_bytes).unwrap();
    assert_eq!(elements.len(), 2);
    assert_eq!(elements.get(0).s().bytes(), b"a");
    assert_eq!(elements.get(1).s().bytes(), b"c");
    let parent_bytes = g("parent_new");
    let parent = PsszView::<Parent0, F>::new(&parent_bytes).unwrap();
    assert_eq!(parent.child().a().get(), 1);
    assert_eq!(parent.child().s().bytes(), b"a");
    assert_eq!(parent.tail().bytes(), b"tail");
    let checked = PsszView::<R0, F>::new_checked(&newest).unwrap();
    assert!(checked.has_unknown);
    // A view over unvalidated malformed bytes errors instead of panicking.
    let w = width::<F>();
    let broken = Frame::new(w).o(4 + w).u32(7).o(200).raw("hi").bytes;
    let view = PsszView::<R0, F>::trusted(&broken);
    assert!(view.try_s().is_err());
    assert!(PsszView::<R0, F>::trusted(&[]).try_a().is_err());
}
each_format!(views_read_evolved_records_in_every_profile, views_read_evolved_records);

// ── Rule 3: optional presence; zero-size and fixed-layout records ───────────

fn presence_and_zero_size_records<F: PsszFormat>() {
    let w = width::<F>();
    // A zero-field extensible record has an inferred bound of 4 (its header
    // bookkeeping), so in the automatic profile its own header is one byte.
    let empty_width = <Empty as PsszPack<F>>::OFFSET_WIDTH;
    assert_eq!(empty_width, if F::AUTO { 1 } else { w });
    let header = |n: usize| Frame::new(empty_width).o(n).bytes;
    // Zero-field extensible record: a bare header. Zero-field DWNC: nothing.
    reads::<F, _>(&header(0), &Empty {}, false);
    reads::<F, _>(&[], &Unit {}, false);
    rejects::<F, Unit>(&[0]);
    rejects::<F, Empty>(&[]);
    reads::<F, _>(&[3, 4], &Pair { x: 3, y: 4 }, false);
    rejects::<F, Pair>(&[3, 4, 5]);
    rejects::<F, Pair>(&[3]);
    // Selector iff the payload can be empty.
    reads::<F, Option<Unit>>(&[0], &None, false);
    reads::<F, _>(&[1], &Some(Unit {}), false);
    rejects::<F, Option<Unit>>(&[]);
    rejects::<F, Option<Unit>>(&[2]);
    rejects::<F, Option<Unit>>(&[0, 0]);
    rejects::<F, Option<Unit>>(&[1, 0]);
    reads::<F, Option<Empty>>(&[], &None, false);
    reads::<F, _>(&header(0), &Some(Empty {}), false);
    reads::<F, Option<Pair>>(&[], &None, false);
    reads::<F, _>(&[3, 4], &Some(Pair { x: 3, y: 4 }), false);
    rejects::<F, Option<Pair>>(&[3]);
    reads::<F, Option<RawString>>(&[0], &None, false);
    reads::<F, _>(&[1], &Some(RawString::new()), false);
    rejects::<F, Option<RawString>>(&[]);
    rejects::<F, Option<RawString>>(&[0, b'x']);
    reads::<F, Option<Option<u32>>>(&[0], &None, false);
    reads::<F, Option<Option<u32>>>(&[1], &Some(None), false);
    reads::<F, Option<Option<RawString>>>(&[], &None, false);
    reads::<F, Option<Option<RawString>>>(&[0], &Some(None), false);
    reads::<F, Option<[u8; 0]>>(&[0], &None, false);
    reads::<F, _>(&[1], &Some([0u8; 0]), false);
    reads::<F, Option<Color>>(&[], &None, false);
    reads::<F, _>(&[7], &Some(Color::Blue), false);
    // Vectors of zero-size elements cannot carry a count.
    reads::<F, Vec<Unit>>(&[], &vec![], false);
    assert!(try_encode::<F, _>(&vec![Unit {}]).is_err());
    reads::<F, _>(&[], &[Unit {}, Unit {}], false);
    rejects::<F, [Unit; 2]>(&[0]);

    // An optional record in the middle keeps its slot; trailing optionals of
    // every presence class are pruned or kept as values.
    let middle = Middle { a: 1, inner: None, s: raw("s") };
    let wire = try_encode::<F, _>(&middle).unwrap();
    assert_eq!(wire, Frame::new(w).o(4 + 2 * w).u32(1).o(4 + 2 * w).o(4 + 2 * w).raw("s").bytes);
    reads::<F, _>(&wire, &middle, false);
    let tail = |unit, empty, pair| Tail { a: 7, s: raw("hi"), unit, empty, pair };
    let base = try_encode::<F, _>(&r0()).unwrap();
    assert_eq!(try_encode::<F, _>(&tail(None, None, None)).unwrap(), base);
    for value in [
        tail(Some(Unit {}), None, None),
        tail(None, Some(Empty {}), None),
        tail(None, None, Some(Pair { x: 1, y: 2 })),
        tail(Some(Unit {}), Some(Empty {}), Some(Pair { x: 1, y: 2 })),
    ] {
        let wire = try_encode::<F, _>(&value).unwrap();
        assert_ne!(wire, base);
        reads::<F, _>(&wire, &value, false);
    }
    let k = 4 + 2 * w;
    assert_eq!(
        try_encode::<F, _>(&tail(Some(Unit {}), None, None)).unwrap(),
        Frame::new(w).o(k).u32(7).o(k).o(k + 2).raw("hi").byte(1).bytes
    );
    // Stored None for the unit optional (selector 00) must have been pruned.
    rejects::<F, Tail>(&Frame::new(w).o(k).u32(7).o(k).o(k + 2).raw("hi").byte(0).bytes);

    // DWNC with a variable field: no header, offset slots, never pruned.
    let dv = DwncVar { a: 9, s: raw("xy"), b: 0x0102 };
    let wire = try_encode::<F, _>(&dv).unwrap();
    assert_eq!(wire, Frame::new(w).byte(9).o(3 + w).byte(2).byte(1).raw("xy").bytes);
    reads::<F, _>(&wire, &dv, false);
    assert!(!<DwncVar as PsszPack<F>>::IS_FIXED_SIZE);
    assert!(<Pair as PsszPack<F>>::IS_FIXED_SIZE);
    assert_eq!(<Unit as PsszPack<F>>::MIN_ENCODED_SIZE, 0);
    assert!(<Empty as PsszPack<F>>::MIN_ENCODED_SIZE > 0);
}
each_format!(presence_and_zero_size_records_in_every_profile, presence_and_zero_size_records);

// ── Rule 4: exact spans ─────────────────────────────────────────────────────

fn exact_spans<F: PsszFormat>() {
    rejects::<F, u16>(&[1, 0, 99]);
    rejects::<F, u16>(&[1]);
    rejects::<F, bool>(&[2]);
    rejects::<F, bool>(&[1, 0]);
    rejects::<F, [u16; 2]>(&[1, 0, 2]);
    rejects::<F, Vec<u32>>(&[1, 0, 0, 0, 2]);
    rejects::<F, Color>(&[1, 0]);
    rejects::<F, Wide>(&[1]);
    reads::<F, _>(&[1, 0], &1u16, false);
    // Tuples are extensible records.
    let w = width::<F>();
    reads::<F, _>(&Frame::new(width_of::<F, ()>()).o(0).bytes, &(), false);
    let t = (7u32, raw("hi"));
    let wire = Frame::new(w).o(4 + w).u32(7).o(4 + w).raw("hi").bytes;
    reads::<F, _>(&wire, &t, false);
    assert_eq!(try_encode::<F, _>(&(7u32, raw("hi"), None::<u8>)).unwrap(), wire);
    reads::<F, _>(&wire, &(7u32, raw("hi"), None::<u8>), false);
    let longer = try_encode::<F, _>(&(7u32, raw("hi"), Some(5u8))).unwrap();
    reads::<F, _>(&longer, &t, true);
    let mut trailing = wire.clone();
    trailing.push(0);
    reads::<F, _>(&trailing, &(7u32, raw("hi\0")), false);
    rejects::<F, (u32, u8)>(
        &Frame::new(width_of::<F, (u32, u8)>()).o(5).u32(7).byte(1).byte(2).bytes,
    );
}
each_format!(exact_spans_in_every_profile, exact_spans);

// ── Rule 6: strings ─────────────────────────────────────────────────────────

fn string_types<F: PsszFormat>() {
    for good in ["", "hello", "h\u{e9}llo \u{1f600}"] {
        reads::<F, _>(good.as_bytes(), &good.to_string(), false);
        reads::<F, _>(good.as_bytes(), &raw(good), false);
        assert_eq!(try_encode::<F, _>(good).unwrap(), good.as_bytes());
        assert_eq!(try_encode::<F, _>(&RawStr(good.as_bytes())).unwrap(), good.as_bytes());
    }
    for bad in [&[0xc0u8, 0x80][..], &[0xff][..]] {
        // UTF-8 types reject; raw strings pass the bytes through.
        rejects::<F, String>(bad);
        rejects::<F, BoundedString<8>>(bad);
        rejects::<F, Vec<String>>(&[&Frame::new(width::<F>()).o(width::<F>()).bytes[..], bad].concat());
        rejects::<F, Option<String>>(&[&[1u8][..], bad].concat());
        assert_eq!(decode::<F, RawString>(bad).unwrap(), RawString(bad.to_vec()));
        assert_eq!(validate::<F, RawString>(bad), Ok(()));
        assert_eq!(try_encode::<F, _>(&RawString(bad.to_vec())).unwrap(), bad);
        assert_eq!(decode::<F, BoundedRawString<8>>(bad).unwrap().0, bad);
        // The exact (strict) readers check UTF-8 in raw strings too, as C++
        // validate_strict does.
        assert!(validate_strict::<F, RawString>(bad).is_err());
        assert!(decode_strict::<F, RawString>(bad).is_err());
        // Views: a String view errors (never panics); a raw view is bytes.
        assert!(PsszView::<String, F>::trusted(bad).view().is_err());
        assert_eq!(PsszView::<RawString, F>::trusted(bad).bytes(), bad);
        assert!(PsszView::<String, F>::new(bad).is_err());
        assert!(PsszView::<RawString, F>::new(bad).is_ok());
    }
    // Bounds: exactly N accepted, N + 1 rejected, on encode and decode.
    reads::<F, _>(b"abcd", &BoundedString::<4>("abcd".into()), false);
    rejects::<F, BoundedString<4>>(b"abcde");
    assert!(try_encode::<F, _>(&BoundedString::<4>("abcde".into())).is_err());
    reads::<F, _>(b"abcd", &BoundedRawString::<4>(b"abcd".to_vec()), false);
    assert!(try_encode::<F, _>(&BoundedRawString::<4>(b"abcde".to_vec())).is_err());
    assert!(decode::<F, BoundedRawString<4>>(b"abcde").is_err());

    let texts = Texts {
        utf8: "\u{e9}".into(),
        rawstr: RawString(vec![0xff]),
        bounded: BoundedString("ab".into()),
        raw_bounded: BoundedRawString(vec![0xc0, 0x80]),
    };
    let wire = try_encode::<F, _>(&texts).unwrap();
    assert_eq!(validate::<F, Texts>(&wire), Ok(()));
    assert_eq!(decode::<F, Texts>(&wire).unwrap(), texts);
    let view = PsszView::<Texts, F>::new(&wire).unwrap();
    assert_eq!(view.utf8().view().unwrap(), "\u{e9}");
    assert_eq!(view.rawstr().bytes(), [0xff]);
    assert_eq!(view.bounded().view().unwrap(), "ab");
    // Moving the invalid bytes into the UTF-8 field is rejected.
    let w = <Texts as PsszPack<F>>::OFFSET_WIDTH;
    let swapped = Frame::new(w).o(4 * w).o(4 * w).o(4 * w + 1).o(4 * w + 3).o(4 * w + 5)
        .byte(0xff).raw("\u{e9}").raw("ab").raw("cd").bytes;
    rejects::<F, Texts>(&swapped);
}
each_format!(string_types_in_every_profile, string_types);

// ── Rule 7: enums ───────────────────────────────────────────────────────────

fn enums<F: PsszFormat>() {
    reads::<F, _>(&[7], &Color::Blue, false);
    reads::<F, _>(&[0x34, 0x12], &Wide::B, false);
    reads::<F, _>(&[0xfe, 0xff, 0xff, 0xff], &Signed::Minus, false);
    reads::<F, _>(&[8, 7, 6, 5, 4, 3, 2, 1], &Big::High, false);
    // A value that is not an enumerator cannot be a Rust enum: rejected.
    rejects::<F, Color>(&[2]);
    rejects::<F, Wide>(&[2, 0]);
    assert!(<Color as PsszPack<F>>::IS_FIXED_SIZE);
    assert_eq!(<Wide as PsszPack<F>>::FIXED_SIZE, 2);
    assert_eq!(<Big as PsszPack<F>>::AUTO_BOUND, Some(8));
    // Inline in a record's fixed region; a vector of them is flat.
    let w = width::<F>();
    let painted = Painted { color: Color::Green, name: raw("n"), wide: Some(Wide::A) };
    let wire = try_encode::<F, _>(&painted).unwrap();
    assert_eq!(wire, Frame::new(w).o(1 + 2 * w).byte(1).o(1 + 2 * w).o(2 + 2 * w).raw("n").byte(1).byte(0).bytes);
    reads::<F, _>(&wire, &painted, false);
    reads::<F, _>(&[0, 7, 1], &vec![Color::Red, Color::Blue, Color::Green], false);
    reads::<F, _>(&[1, 0, 0x34, 0x12], &[Wide::A, Wide::B], false);

    // Data-carrying enums are variants: one index byte, then the alternative.
    reads::<F, _>(&[0], &Shape3::Nothing, false);
    reads::<F, _>(b"\x01hi", &Shape3::Text("hi".into()), false);
    let point = [&[2u8][..], &Frame::new(w).o(1 + w).byte(5).o(1 + w).raw("p").bytes].concat();
    reads::<F, _>(&point, &Shape3::Point(5, raw("p")), false);
    // Several fields form a positional tuple record, with pruning.
    assert_eq!(try_encode::<F, _>(&(5u8, raw("p"))).unwrap(), point[1..]);
    let named = [
        &[3u8][..],
        &Frame::new(width_of::<F, (u16, Option<u8>)>()).o(2).byte(9).byte(0).bytes,
    ]
    .concat();
    reads::<F, _>(&named, &Shape3::Named { a: 9, b: None }, false);
    rejects::<F, Shape3>(&[]);
    rejects::<F, Shape3>(&[4]);
    rejects::<F, Shape3>(&[0, 0]);
    rejects::<F, Shape3>(&[1, 0xff]);
    assert!(!<Shape3 as PsszPack<F>>::IS_FIXED_SIZE);
    // A variant can never be empty: Option<variant> has no selector.
    reads::<F, Option<Shape3>>(&[], &None, false);
    reads::<F, _>(&[0], &Some(Shape3::Nothing), false);
}
each_format!(enums_in_every_profile, enums);

// ── Deliverable B: compile-time evolution check ─────────────────────────────

const _: () = {
    assert!(pssz_evolves_from::<R0, R0>());
    assert!(pssz_evolves_from::<R0, R1>());
    assert!(pssz_evolves_from::<R0, R3>());
    assert!(pssz_evolves_from::<R1, R3>());
    assert!(pssz_evolves_from::<R0, N1>());
    assert!(pssz_evolves_from::<R0, P1>());
    assert!(pssz_evolves_from::<Parent0, Parent2>());
    assert!(pssz_evolves_from::<Vec<R0>, Vec<R2>>());
    assert!(pssz_evolves_from::<Option<R0>, Option<R2>>());
    assert!(pssz_evolves_from::<VarOld, VarNew>());
    assert!(pssz_evolves_from::<(u32, R0), (u32, R2, Option<u8>)>());
    // Appended field is not optional; fields removed; field changed.
    assert!(!pssz_evolves_from::<R0, Bad1>());
    assert!(!pssz_evolves_from::<R1, R0>());
    assert!(!pssz_evolves_from::<R0, Inner>());
    // An explicit cap keeps the width through growth...
    assert!(pssz_evolves_from::<WidthGrowV1, WidthGrowV2>());
    assert!(pssz_evolves_from::<WidthGrowParentV1, WidthGrowParentV2>());
    // ...whereas an inferred bound may cross a threshold: {[u8; 253]} is
    // one byte wide (1 + 253) and becomes two with an appended Option<u8>
    // (2 + 253 + 2 = 257 > 255).
    assert!(!pssz_evolves_from::<WidthThreshold253, WidthGrown253>());
    // Fixed-layout records never gain fields; DWNC status cannot change.
    assert!(pssz_evolves_from::<Pair, Pair>());
    assert!(!pssz_evolves_from::<Unit, Empty>());
    assert!(!pssz_evolves_from::<WidthStringPair, WidthCappedPair>());
    assert!(!pssz_evolves_from::<Color, Wide>());
    assert!(!pssz_evolves_from::<String, RawString>());
};

#[test]
fn evolves_from_is_also_a_runtime_predicate() {
    assert!(pssz_evolves_from::<R0, R2>());
    assert!(!pssz_evolves_from::<R2, R0>());
    assert_eq!(auto_pssz_width::<WidthThreshold253>(), 1);
    assert_eq!(auto_pssz_width::<WidthGrown253>(), 2);
}
