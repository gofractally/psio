// Included by ../pssz_conformance.rs inside each record-flavour module.

macro_rules! conf_record {
    ($kind:literal, $($cap:literal)?; $name:ident { $($f:ident : $t:ty),* }) => {
        impl Conf for $name {
            fn id() -> String { stringify!($name).into() }
            fn shape() -> String {
                let fields: Vec<String> = vec![$(format!(
                    "{}:{}", stringify!($f).trim_end_matches('_'), <$t as Conf>::id())),*];
                #[allow(unused_mut)]
                let mut out = format!("{}{{{}}}", $kind, fields.join(","));
                $( out += &format!("@cap={}", $cap); )?
                out
            }
        }
    };
}

include!("types.rs");
include!("registry.rs");

/// Classes where C++ and Rust are known to differ by type design. The
/// fixture records what C++ does; an entry here would be the outcome Rust
/// must have instead. There are none: the one former difference, an enum
/// value that names no enumerator (`enum-unknown-value`), is taken by the
/// mirror enums' `#[psio(other)]` catch-all, as the fixture expects.
fn rust_expectation(v: &Vector) -> &str {
    v.expect.as_str()
}

#[test]
fn every_fixture_type_is_mapped_to_the_same_logical_type() {
    let fixture = fixture();
    let registry = registry();
    let mut problems = vec![];
    for info in &fixture.types {
        match registry.get(&info.id) {
            None => problems.push(format!("unmapped type id: {}", info.id)),
            Some(entry) => {
                if let Err(e) = (entry.manifest)(info) {
                    problems.push(format!("{}: {e}", info.id));
                }
            }
        }
    }
    assert!(problems.is_empty(), "{} problems:\n{}", problems.len(), problems.join("\n"));
    assert!(fixture.types.len() >= 100, "fixture type manifest shrank");
}

#[test]
fn every_vector_reaches_the_fixture_outcome_on_every_entry_point() {
    let fixture = fixture();
    let registry = registry();
    let mut failures = vec![];
    let mut divergent = 0usize;
    for v in &fixture.vectors {
        let Some(entry) = registry.get(&v.ty) else {
            failures.push(format!("{}: unmapped type id {}", v.name, v.ty));
            continue;
        };
        let mut expected = v.clone();
        let rust = rust_expectation(v).to_string();
        if rust != v.expect {
            divergent += 1;
            expected.expect = rust;
        }
        if let Err(e) = (entry.check)(&expected) {
            failures.push(format!(
                "{} [{} {} {} expect={}] {}: {e}",
                v.name, v.class, v.ty, v.format, expected.expect, v.hex
            ));
        }
    }
    let shown: Vec<_> = failures.iter().take(40).cloned().collect();
    assert!(
        failures.is_empty(),
        "{} of {} vectors failed:\n{}",
        failures.len(),
        fixture.vectors.len(),
        shown.join("\n")
    );
    assert!(fixture.vectors.len() >= 1000, "fixture shrank");
    // No vector has a Rust outcome that differs from the fixture's. The
    // undeclared enum values, once a documented difference, are accepted on
    // every entry point (checked above against `canonical`). A member
    // length_bound is enforced by both languages (`length-bound-overflow`
    // and `width/cap/checkpoint_at_cap` are rejections in the fixture).
    assert_eq!(divergent, 0, "there is no documented C++/Rust divergence");
    let unknown_enum_values: Vec<_> =
        fixture.vectors.iter().filter(|v| v.class == "enum-unknown-value").collect();
    assert_eq!(unknown_enum_values.len(), 36, "the class has exactly 36 vectors");
    assert!(unknown_enum_values.iter().all(|v| v.expect == "canonical"));
    for v in &unknown_enum_values {
        let entry = registry.get(&v.ty).expect("mapped above");
        assert_eq!((entry.check)(v), Ok(()), "{}: not accepted", v.name);
    }
    let bound_rejections = fixture
        .vectors
        .iter()
        .filter(|v| {
            v.class == "length-bound-overflow"
                || v.name.starts_with("width/cap/checkpoint_at_cap/")
        })
        .collect::<Vec<_>>();
    assert_eq!(bound_rejections.len(), 36);
    assert!(bound_rejections.iter().all(|v| v.expect == "reject"));
    // Every vector of every class was checked above: none is skipped.
    let classes: std::collections::BTreeMap<&str, usize> =
        fixture.vectors.iter().fold(Default::default(), |mut count, v| {
            *count.entry(v.class.as_str()).or_default() += 1;
            count
        });
    for (class, count) in [
        ("nested", 252), ("nested-unknown", 20), ("nested-deferred", 24), ("map-set", 266),
        ("map-set-order", 352), ("transparent", 160), ("custom", 116), ("invariant", 128),
    ] {
        assert_eq!(classes.get(class), Some(&count), "{class}");
    }
    assert_eq!(fixture.vectors.len(), 6589);
    // Every invariant rejection carries the invariant's own message, and
    // every order rejection the order rule's: compared word for word above.
    let shared: Vec<_> = fixture.vectors.iter().filter(|v| v.reason_is_shared() && v.expect == "reject").collect();
    assert_eq!(shared.len(), 128 + 180);
    assert!(shared.iter().all(|v| !v.reason.is_empty()));
}

#[test]
fn the_enum_ir_rust_builds_is_the_ir_cpp_recorded() {
    use psio::pssz::schema_ir::{schema_for, AnyValue, Schema, ToPsszSchema};
    let fixture = fixture();
    let bytes = unhex(&fixture.schema_ir);
    let recorded = from_pssz::<Schema>(&bytes).expect("schema_ir decodes");
    // The decoded IR re-encodes to the fixture's bytes: Enum included.
    assert_eq!(try_to_pssz(&recorded).unwrap(), bytes);
    fn check<T: ToPsszSchema + Conf>(recorded: &Schema) {
        let id = T::id();
        let (built, _) = schema_for::<T>().unwrap();
        // The fixture lists each type under its id, as an alias of the name
        // the C++ builder gave it.
        let rust = built.get(&id).expect("built");
        let cpp = recorded.resolve(recorded.get(&id).expect("recorded")).expect("resolves");
        assert!(matches!(cpp.value, AnyValue::Enum(_)), "{id}: C++ recorded an Enum");
        assert_eq!(rust, cpp, "{id}");
        assert_eq!(try_to_pssz(rust).unwrap(), try_to_pssz(cpp).unwrap(), "{id}");
    }
    check::<Color>(&recorded);
    check::<E16>(&recorded);
    check::<E32>(&recorded);
    check::<E64>(&recorded);
}

#[test]
fn the_fixture_covers_every_required_class_in_every_profile() {
    let fixture = fixture();
    for class in [
        "corpus", "evolution-forward", "evolution-backward", "pruning", "malformed",
        "optional-presence", "width", "tuple", "variant", "enum", "enum-unknown-value",
        "string", "length-bound-overflow", "bytes", "zero-fixed", "depth",
        "nested", "nested-unknown", "nested-deferred", "map-set", "map-set-order", "transparent",
        "custom", "invariant",
    ] {
        let of_class: Vec<_> = fixture.vectors.iter().filter(|v| v.class == class).collect();
        assert!(!of_class.is_empty(), "no vectors of class {class}");
        assert!(of_class.iter().any(|v| v.format == "auto"), "{class}: no automatic-profile vector");
    }
    for expect in ["canonical", "unknown", "reject"] {
        for format in ["auto", "pssz8", "pssz16", "pssz32"] {
            assert!(
                fixture.vectors.iter().any(|v| v.expect == expect && v.format == format),
                "no {expect} vector for {format}"
            );
        }
    }
    let mut names = std::collections::BTreeSet::new();
    for v in &fixture.vectors {
        assert!(names.insert(&v.name), "duplicate vector name {}", v.name);
    }
}

#[test]
fn truncated_and_corrupted_fixture_frames_never_panic_or_decode_unvalidated() {
    let fixture = fixture();
    let registry = registry();
    let mut cases = 0usize;
    let mut failures = vec![];
    for v in fixture.vectors.iter().filter(|v| v.expect == "canonical" && v.hex.len() <= 160) {
        if rust_expectation(v) != "canonical" {
            continue;
        }
        let Some(entry) = registry.get(&v.ty) else { continue };
        match std::panic::catch_unwind(|| (entry.sweep)(v)) {
            Ok(Ok(n)) => cases += n,
            Ok(Err(e)) => failures.push(format!("{}: {e}", v.name)),
            Err(_) => failures.push(format!("{}: PANIC", v.name)),
        }
    }
    let shown: Vec<_> = failures.iter().take(20).cloned().collect();
    assert!(failures.is_empty(), "{} failures:\n{}", failures.len(), shown.join("\n"));
    assert!(cases > 10_000, "only {cases} mutated frames were exercised");
}

// ── Reverse direction: values written natively in Rust ──────────────────────
//
// The checks above start from C++ bytes. Here the same logical values are
// constructed from Rust literals (mirroring cpp/tests/pssz_conformance_emit.cpp)
// and Rust's encoding must equal the fixture bytes in every profile.

fn raw(s: &str) -> Raw { RawString::from(s) }
fn bleaf(n: u16, t: &str) -> BLeaf { BLeaf { n, t: BoundedString(t.into()) } }
fn native_values() -> Vec<(&'static str, (String, Native))> {
    let cp = |height, label: &str| WidthCheckpoint { height, label: BoundedRawString(label.as_bytes().to_vec()) };
    let r2 = |x, y| R2 { a: 7, s: raw("hi"), x, y };
    let new = R2 { a: 1, s: raw("a"), x: Some(9), y: Some(raw("b")) };
    let inner = || Inner { k: 3, t: raw("t") };
    let u256 = Uint256 { limb: [0; 4] };
    vec![
        // Evolution lineages.
        ("r0", native(R0 { a: 7, s: raw("hi") })),
        ("r1_none", native(R1 { a: 7, s: raw("hi"), x: None })),
        ("r1_some", native(R1 { a: 7, s: raw("hi"), x: Some(9) })),
        ("r2_none_none", native(r2(None, None))),
        ("r2_some_none", native(r2(Some(9), None))),
        ("r2_none_some", native(r2(None, Some(raw("yo"))))),
        ("r2_none_empty", native(r2(None, Some(raw(""))))),
        ("r3_none", native(R3 { a: 7, s: raw("hi"), x: None, y: None, z: None })),
        ("r3_all", native(R3 { a: 7, s: raw("hi"), x: Some(9), y: Some(raw("yo")), z: Some(vec![1, 2]) })),
        ("r3_mid", native(R3 { a: 7, s: raw("hi"), x: None, y: Some(raw("yo")), z: None })),
        ("n1_none", native(N1 { a: 7, s: raw("hi"), in_: None })),
        ("n1_some", native(N1 { a: 7, s: raw("hi"), in_: Some(inner()) })),
        ("n2_oo_none", native(N2 { a: 7, s: raw("hi"), in_: None, oo: Some(None) })),
        ("n2_both", native(N2 { a: 7, s: raw("hi"), in_: Some(inner()), oo: Some(Some(5)) })),
        ("e1_none", native(E1 { a: 7, s: raw("hi"), e: None })),
        ("e1_some", native(E1 { a: 7, s: raw("hi"), e: Some(Color::Green) })),
        ("e1_red", native(E1 { a: 7, s: raw("hi"), e: Some(Color::Red) })),
        ("e2_var_u32", native(E2 { a: 7, s: raw("hi"), e: None, v: Some(V2::A(5)) })),
        ("e2_var_string", native(E2 { a: 7, s: raw("hi"), e: Some(Color::Blue), v: Some(V2::B(raw("v"))) })),
        ("e3_tuple", native(E3 { a: 7, s: raw("hi"), e: None, v: None, t: Some((1, raw("t"))) })),
        ("e3_all", native(E3 { a: 7, s: raw("hi"), e: Some(Color::Green), v: Some(V2::B(raw(""))), t: Some((0, raw(""))) })),
        ("vec_new", native(vec![new.clone(), R2 { a: 2, s: raw("c"), x: None, y: None }])),
        ("opt_new", native(Some(new.clone()))),
        ("var_new", native(V2::<u32, R2>::B(new.clone()))),
        ("parent_new", native(Parent2 { child: new.clone(), tail: raw("tail") })),
        // Type-derived widths.
        ("relay_sample", native(WidthRelayBatch {
            checkpoint: cp(7, "main"),
            entries: vec![cp(8, "one"), cp(9, "two")],
            latest: Some(cp(10, "last")),
            pair: [cp(11, "left"), cp(12, "right")],
            proof: V2::A(cp(13, "proof")),
            context: raw("relay"),
        })),
        ("relay_empty", native(WidthRelayBatch {
            checkpoint: cp(0, ""), entries: vec![], latest: None,
            pair: [cp(0, ""), cp(0, "")], proof: V2::B(42), context: raw(""),
        })),
        ("checkpoint_full_label", native(cp(0xffff_ffff, "0123456789abcdef"))),
        ("checkpoint_vector", native(vec![cp(8, "one"), cp(9, "two")])),
        ("checkpoint_array", native([cp(11, "left"), cp(12, "right")])),
        ("checkpoint_some", native(Some(cp(10, "last")))),
        ("checkpoint_none", native(None::<WidthCheckpoint>)),
        ("checkpoint_variant_record", native(V2::<WidthCheckpoint, u32>::A(cp(13, "proof")))),
        ("checkpoint_variant_number", native(V2::<WidthCheckpoint, u32>::B(42))),
        ("narrow_envelope", native(WidthNarrowEnvelope { child: WidthLargeBudget { value: 17 }, sequence: 42 })),
        ("large_budget", native(WidthLargeBudget { value: 17 })),
        ("reserve_parent_17", native(WidthReserveParent { child: WidthLargeBudget { value: 17 } })),
        ("threshold_251", native(WidthThreshold251 { value: [0; 251] })),
        ("threshold_252", native(WidthThreshold252 { value: [0; 252] })),
        ("narrow_batch_full", native(NarrowBatch { entries: BoundedList(vec![Tiny { v: 1 }, Tiny { v: 2 }]) })),
        ("t16_full", native(T16 { a: 7, s: BoundedString("0123456789abcdef".into()) })),
        ("t300_full", native(T300 { a: 7, s: BoundedString("s".repeat(300)) })),
        ("s247_max", native(S247 { s: BoundedRawString(vec![b's'; 247]) })),
        ("capped_pair_at_cap", native(WidthCappedPair { first: RawString(vec![b'a'; 126]), second: RawString(vec![b'b'; 126]) })),
        // Strings, bytes, scalars, enums, tuples, variants, zero-size records.
        ("strings", native(StrRec { r: raw("raw"), t: "h\u{e9}".into(), b: BoundedString("abcd".into()) })),
        ("strings_empty", native(StrRec { r: raw(""), t: String::new(), b: BoundedString(String::new()) })),
        ("string_list", native(vec!["a".to_string(), String::new(), "\u{e9}".to_string()])),
        ("bounded_within", native(Bounded { s: BoundedRawString(b"1234".to_vec()), v: BoundedList(vec![1, 2]) })),
        ("byte_list", native(vec![1u8, 2, 3])),
        ("byte_array", native([1u8, 2, 3, 4])),
        ("bitvector_pattern", native(Bitvector::<12> { bytes: vec![0x01, 0x08] })),
        ("bitlist_seven", native(Bitlist::<12> { bits: vec![true, false, true, false, true, false, true] })),
        ("u256_zero", native(u256)),
        ("u128_max", native(u128::MAX)),
        ("i128_minus_two", native(-2i128)),
        ("u64_pattern", native(0x0102_0304_0506_0708u64)),
        ("i32_min", native(i32::MIN)),
        ("f32_negative_zero", native(-0.0f32)),
        ("f64_minus_two_and_a_half", native(-2.5f64)),
        ("bool_true", native(true)),
        ("u32_list", native(vec![1u32, 2])),
        ("record", native(EnumRec {
            c: Color::Blue, w: E16::Hi, i: E32::Neg, q: E64::Big,
            list: vec![Color::Green, Color::Red], opt: Some(E32::Pos),
        })),
        ("record_pruned", native(EnumRec {
            c: Color::Red, w: E16::Lo, i: E32::Zero, q: E64::Zero, list: vec![], opt: None,
        })),
        ("index0_scalar", native(Var5::A(5))),
        ("index1_string", native(Var5::B(raw("hi")))),
        ("index2_record", native(Var5::C(inner()))),
        ("index3_tuple", native(Var5::D((1, raw("t"))))),
        ("index4_unit", native(Var5::E(Unit {}))),
        ("u8_or_string", native(VarBS::B(raw("hi")))),
        ("three_optional_some", native((7u32, raw("hi"), Some(9u16)))),
        ("three_optional_none", native((7u32, raw("hi"), None::<u16>))),
        ("dwnc_with_variable_field", native(DwncVar { k: 7, s: raw("hi") })),
        ("vector_of_dwnc_variable", native(vec![DwncVar { k: 7, s: raw("hi") }, DwncVar { k: 8, s: raw("") }])),
        // Boxed values.
        ("leaf", native(bleaf(7, "abcd"))),
        ("direct", native(BDirect { id: 1, leaf: bleaf(2, "ab") })),
        ("record", native(BoxRec { id: 1, leaf: Box::new(bleaf(2, "ab")) })),
        ("record_capped", native(BoxCapRec { id: 1, leaf: Box::new(bleaf(2, "ab")) })),
        ("optional_some", native(BoxOptRec { id: 3, leaf: Some(Box::new(bleaf(4, "xyz"))), tail: 9 })),
        ("optional_none", native(BoxOptRec { id: 3, leaf: None, tail: 9 })),
        ("vector_three", native(BoxVecRec {
            id: 5,
            leaves: BoundedList(vec![Box::new(bleaf(1, "a")), Box::new(bleaf(2, "")), Box::new(bleaf(3, "abcd"))]),
        })),
        ("vector_capped", native(BoxVecCapRec { id: 5, leaves: vec![Box::new(bleaf(1, "a")), Box::new(bleaf(2, "bc"))] })),
        ("fixed", native(BoxFixedRec { id: 1, word: Box::new(0x0102_0304), pair: Box::new([5, 6]) })),
        ("outer", native(BoxOuter {
            k: 1,
            capped: BoxCapRec { id: 2, leaf: Box::new(bleaf(3, "c")) },
            open: BoxRec { id: 4, leaf: Box::new(bleaf(5, "open")) },
            direct: Some(BDirect { id: 6, leaf: bleaf(7, "d") }),
        })),
        ("top_level", native(Box::new(bleaf(9, "top")))),
        ("in_vector", native(vec![Box::new(bleaf(1, "a")), Box::new(bleaf(2, "bb"))])),
        ("in_optional", native(Some(Box::new(bleaf(1, "a"))))),
        ("in_variant", native(V2::<Box<BLeaf>, u8>::A(Box::new(bleaf(1, "v"))))),
        ("in_tuple", native((Box::new(bleaf(1, "t")), 2u8))),
        ("depth_3", native(DepthNode { value: 1, next: Some(Box::new(DepthNode { value: 7, next: None })) })),
        // Nested values: the bytes of the byte list holding the encoding.
        ("args", native(nargs())),
        ("action", native(NAction { method: 9, args: Nested::new(&nargs()).unwrap() })),
        ("action", native(NActionRaw { method: 9, args: to_pssz(&nargs()) })),
        ("action", native(NActionLater { method: 9, args: Nested::new(&nargs()).unwrap() })),
        ("scalar", native(Nested::<u32>::new(&0x0102_0304).unwrap())),
        ("record", native(Nested::<NArgs>::new(&nargs()).unwrap())),
        ("list_two", native(vec![Nested::<u32>::new(&1).unwrap(), Nested::new(&2).unwrap()])),
        ("optional_some", native(Some(Nested::<Raw>::new(&raw("hi")).unwrap()))),
        ("variant_nested", native(V2::<u8, Nested<NArgs>>::B(Nested::new(&nargs()).unwrap()))),
        ("capped_at_bound", native(NCapped { id: 1, note: Nested::new(&raw("12345678")).unwrap() })),
        ("chain_3", native(chain(3))),
        ("nested_map", native(Nested::<BTreeMap<u32, Raw>>::new(
            &[(256, raw("x")), (1, raw("one"))].into_iter().collect()).unwrap())),
        // Pairs, maps and sets: the canonical order whatever the container.
        ("pair_literal", native((7u16, raw("ab")))),
        ("map_str_literal", native(
            [(raw("b"), 2u32), (raw("a"), 1)].into_iter().collect::<std::collections::HashMap<Raw, u32>>())),
        ("map_str_literal", native(
            [(raw("b"), 2u32), (raw("a"), 1)].into_iter().collect::<BTreeMap<Raw, u32>>())),
        ("map_u32_extremes", native(
            [(u32::MAX, raw("")), (0, raw("zero"))].into_iter().collect::<BTreeMap<u32, Raw>>())),
        ("set_u16_literal", native([513u16, 2].into_iter().collect::<std::collections::HashSet<u16>>())),
        ("set_i32_signed", native([1, -1, i32::MAX, 0, i32::MIN].into_iter().collect::<BTreeSet<i32>>())),
        ("set_f64_nan_order", native(
            [f64::INFINITY, 0.0, f64::from_bits(0xfff8_0000_0000_0000), 1.0, f64::NEG_INFINITY,
             f64::from_bits(0x7ff8_0000_0000_0000)].map(ByKey).into_iter().collect::<BTreeSet<_>>())),
        ("set_record_many", native(
            [Pair { x: 2, y: 0 }, Pair { x: 1, y: 5 }, Pair { x: 1, y: 4 }].map(ByKey).into_iter()
                .collect::<BTreeSet<_>>())),
        ("set_nested_inner_byte_order", native(
            [1u32, 256, 0].iter().map(|n| Nested::<u32>::new(n).unwrap()).collect::<BTreeSet<_>>())),
        ("variant_map", native(V2::<BTreeMap<Raw, u32>, u32>::A(
            [(raw("a"), 1), (raw("b"), 2)].into_iter().collect()))),
        ("optional_set_some", native(Some([513u16, 2].into_iter().collect::<BTreeSet<u16>>()))),
        // Transparent wrappers: the member's bytes.
        ("id/max", native(Id { value: u64::MAX })),
        ("id/list", native(vec![Id { value: 1 }, Id { value: 2 }, Id { value: u64::MAX }])),
        ("id/some", native(Some(Id { value: 7 }))),
        ("blob/bytes", native(Blob { data: vec![1, 2, 3] })),
        ("ref/name", native(Ref { target: RefTarget::Name(raw("alice")) })),
        ("ref/number", native(Ref { target: RefTarget::Number(42) })),
        ("tag/map_keys", native(
            [(Tag { text: raw("b") }, 2u32), (Tag { text: raw("a") }, 1)].into_iter().collect::<BTreeMap<_, _>>())),
        // Custom types: the wire type's bytes.
        ("endpoint/v4", native(Endpoint::V4(V4 { address: [10, 0, 0, 7], port: 1 }))),
        ("ratio/negative", native(Ratio((-3, 4)))),
        ("name/largest", native(Name { value: (1 << 60) - 1 })),
        ("range/ordered", native(Range { lo: 3, hi: 9 })),
        ("level/array", native([Level::Mid, Level::Top])),
        ("derkey/some", native(Some(DerKey { data: vec![0x30, 0] }))),
        ("map_name_range/two", native(
            [(Name { value: 7 }, Range { lo: 1, hi: 2 }), (Name { value: 3 }, Range { lo: 0, hi: 0 })]
                .into_iter().collect::<BTreeMap<_, _>>())),
        ("set_level/two", native([Level::Top, Level::Low].into_iter().collect::<BTreeSet<_>>())),
        ("nested_range/ordered", native(Nested::<Range>::new(&Range { lo: 3, hi: 9 }).unwrap())),
    ]
}
fn nargs() -> NArgs {
    NArgs { to: raw("bob"), amount: 5, memo: Some(raw("rent")) }
}
/// `links` nested levels, the innermost with no next.
fn chain(links: usize) -> NChain {
    (0..links).fold(NChain { next: None }, |inner, _| NChain {
        next: Some(Nested::new(&inner).unwrap()),
    })
}

// ── Writers ─────────────────────────────────────────────────────────────────
//
// The vectors of class `invariant` are frames "as a writer that does not
// check would produce them", and those of `map-set-order` frames an encoder
// cannot produce from a map / set. The Rust writers:

#[test]
fn a_value_that_violates_an_invariant_has_no_encoding() {
    fn refused<T: All>(value: T, reason: &str) {
        for format in ["auto", "pssz8", "pssz16", "pssz32"] {
            assert_eq!(
                encode::<T>(format, &value).map_err(|e| e.0),
                Err(reason.into()),
                "{} {format}: {value:?}",
                T::id()
            );
        }
    }
    const NAME: &str = "Name: value is not a canonical name";
    const RANGE: &str = "Range: lo is greater than hi";
    const LEVEL: &str = "Level: not a listed level";
    const DER_KEY: &str = "DerKey: not a DER sequence";
    let bad_name = Name { value: 1 << 60 };
    let bad_range = Range { lo: 9, hi: 3 };
    refused(bad_name.clone(), NAME);
    refused(bad_range.clone(), RANGE);
    refused(Level::Unknown(4), LEVEL);
    refused(DerKey { data: vec![] }, DER_KEY);
    refused(DerKey { data: vec![0x31, 0] }, DER_KEY);
    refused(Ref { target: RefTarget::Name(raw("")) }, "Ref: a name is not empty");
    // Where the value occurs: list, array, optional, variant, record member,
    // map key, mapped value, set element.
    refused(vec![Name { value: 1 }, bad_name.clone()], NAME);
    refused([Level::Low, Level::Unknown(200)], LEVEL);
    refused(Some(bad_name.clone()), NAME);
    refused(NameOrRange::Range(bad_range.clone()), RANGE);
    refused([(bad_name.clone(), Range { lo: 0, hi: 0 })].into_iter().collect::<BTreeMap<_, _>>(), NAME);
    refused([(Name { value: 1 }, bad_range.clone())].into_iter().collect::<BTreeMap<_, _>>(), RANGE);
    // The key's violation is reported before its mapped value's.
    refused([(bad_name.clone(), bad_range.clone())].into_iter().collect::<BTreeMap<_, _>>(), NAME);
    refused([Level::Low, Level::Unknown(9)].into_iter().collect::<BTreeSet<_>>(), LEVEL);
    // A nested value is checked when it is made: there is no Nested<Range>
    // holding a reversed range except through `unchecked`, whose bytes every
    // reader then rejects (the vector invariant/nested/range_reversed).
    assert_eq!(Nested::<Range>::new(&bad_range).map_err(|e| e.0), Err(RANGE.into()));
    let adopted = Nested::<Range>::unchecked(vec![8, 9, 0, 0, 0, 3, 0, 0, 0]);
    assert_eq!(adopted.validate().map_err(|e| e.0), Err(RANGE.into()));
    let frame = try_to_pssz(&adopted).expect("a writer does not look inside");
    assert_eq!(from_pssz::<Nested<Range>>(&frame).map_err(|e| e.0), Err(RANGE.into()));
    assert_eq!(Nested::<Range>::checked(frame).map_err(|e| e.0), Err(RANGE.into()));
    // A value that satisfies its invariants is written.
    assert!(try_to_pssz(&Name { value: (1 << 60) - 1 }).is_ok());
}

#[test]
fn a_map_or_set_is_written_in_key_order_and_refuses_equal_keys() {
    use std::collections::{HashMap, HashSet};
    // Numeric order, not the order of the little-endian wire bytes.
    let set: HashSet<u16> = [513, 2].into_iter().collect();
    assert_eq!(hex(&try_to_pssz(&set).unwrap()), "02000102");
    let map: HashMap<String, u32> = [("b".to_string(), 2), ("a".to_string(), 1)].into_iter().collect();
    let bytes = try_to_pssz(&map).unwrap();
    assert_eq!(bytes, try_to_pssz(&vec![("a".to_string(), 1u32), ("b".to_string(), 2)]).unwrap());
    assert_eq!(from_pssz::<HashMap<String, u32>>(&bytes).unwrap(), map);
    // The frame with the two entries exchanged is the list, not the map.
    let swapped = try_to_pssz(&vec![("b".to_string(), 2u32), ("a".to_string(), 1)]).unwrap();
    assert_eq!(
        from_pssz::<HashMap<String, u32>>(&swapped).map_err(|e| e.0),
        Err("pssz: map entries are not in strictly ascending key order".into())
    );
    // Two entries whose keys have equal key bytes are duplicates, whatever
    // the container thinks: `Low` and `Unknown(0)` are one key (the order is
    // computed before, and without, the invariant that refuses the second).
    let twice: BTreeSet<Level> = [Level::Low, Level::Unknown(0)].into_iter().collect();
    assert_eq!(twice.len(), 2);
    assert_eq!(try_to_pssz(&twice).map_err(|e| e.0), Err("pssz: set has duplicate elements".into()));
    let twice: BTreeMap<Level, u8> = [(Level::Low, 1), (Level::Unknown(0), 2)].into_iter().collect();
    assert_eq!(try_to_pssz(&twice).map_err(|e| e.0), Err("pssz: map has duplicate keys".into()));
    // -0 and +0 are one key: a set by key order cannot hold both.
    let zeros: BTreeSet<ByKey<f64>> = [ByKey(0.0), ByKey(-0.0)].into_iter().collect();
    assert_eq!(zeros.len(), 1);
    // A bounded map counts entries.
    let three: BoundedMap<u8, u8, 2> = BoundedMap([(1, 1), (2, 2), (3, 3)].into_iter().collect());
    assert!(try_to_pssz(&three).is_err());
}

#[test]
fn rust_native_encodings_equal_the_cpp_fixture_bytes() {
    let fixture = fixture();
    let values = native_values();
    let mut compared = 0usize;
    let mut failures = vec![];
    for (value_id, (type_id, encode)) in &values {
        let vectors: Vec<_> = fixture
            .vectors
            .iter()
            .filter(|v| v.value == *value_id && v.ty == *type_id && v.writer == v.ty && v.expect == "canonical")
            .collect();
        if vectors.is_empty() {
            failures.push(format!("{value_id} as {type_id}: no fixture vector"));
        }
        for v in vectors {
            match encode(&v.format) {
                Ok(bytes) if hex(&bytes) == v.hex => compared += 1,
                Ok(bytes) => failures.push(format!("{}: rust {} != c++ {}", v.name, hex(&bytes), v.hex)),
                Err(e) => failures.push(format!("{}: rust encode failed: {e}", v.name)),
            }
        }
    }
    assert!(failures.is_empty(), "{} failures:\n{}", failures.len(), failures.join("\n"));
    assert!(compared >= 4 * values.len() - 40, "only {compared} encodings were compared");
}

// ── The type manifest: custom ids and invariants ────────────────────────────

#[test]
fn the_manifest_custom_ids_and_invariants_are_those_of_the_rust_types() {
    use psio::pssz::schema_ir::{schema_for, AnyValue, Schema, ToPsszSchema};
    let fixture = fixture();
    let recorded = from_pssz::<Schema>(&unhex(&fixture.schema_ir)).expect("schema_ir decodes");
    let info = |id: &str| fixture.types.iter().find(|t| t.id == id).unwrap_or_else(|| panic!("{id}"));
    // `custom_id`: the id of the Custom node Rust builds for the type, which
    // is the node C++ recorded (same id, same kind of wire type). A
    // transparent type with neither a text form nor an invariant is a named
    // alias of its member's type: no Custom node.
    fn check<T: ToPsszSchema + Conf>(recorded: &Schema, info: &TypeInfo) {
        let id = T::id();
        assert_eq!(T::custom_id(), info.custom_id, "{id}: custom_id");
        let (built, root) = schema_for::<T>().unwrap();
        let rust = built.resolve(&root).unwrap();
        let cpp = recorded.resolve(recorded.get(&id).expect("recorded")).expect("resolves");
        match (&rust.value, &cpp.value, &info.custom_id) {
            (AnyValue::Custom(rust), AnyValue::Custom(cpp), Some(custom_id)) => {
                assert_eq!(&rust.id, custom_id, "{id}");
                assert_eq!(&cpp.id, custom_id, "{id}");
                let wire = |schema: &Schema, ty| std::mem::discriminant(&schema.wire(ty).unwrap().value);
                assert_eq!(wire(&built, &rust.r#type), wire(recorded, &cpp.r#type), "{id}: wire type");
            }
            (rust, cpp, None) => {
                assert!(!matches!(rust, AnyValue::Custom(c) if c.id == id), "{id}: Rust built a Custom node");
                assert!(!matches!(cpp, AnyValue::Custom(c) if c.id == id), "{id}: C++ recorded a Custom node");
                // The same bytes either way.
                assert_eq!(
                    std::mem::discriminant(&built.wire(&root).unwrap().value),
                    std::mem::discriminant(&recorded.wire(recorded.get(&id).unwrap()).unwrap().value),
                    "{id}: wire type"
                );
            }
            (rust, cpp, Some(_)) => panic!("{id}: not Custom nodes: {rust:?} / {cpp:?}"),
        }
    }
    macro_rules! each { ($($t:ty),*) => { $(check::<$t>(&recorded, info(&<$t as Conf>::id()));)* }; }
    each!(Id, Tag, Blob, Ref, Name, DerKey, Range, Level, V4, V6, Endpoint, Ratio, Guarded);
    let with_id: Vec<&str> =
        fixture.types.iter().filter(|t| t.custom_id.is_some()).map(|t| t.id.as_str()).collect();
    assert_eq!(
        with_id,
        ["Blob", "Ref", "Name", "DerKey", "Range", "Level", "variant<V4,V6>", "tuple<i16,u16>"]
    );
    // `invariant`: the types an invariant constrains are exactly those with
    // vectors of class `invariant`, plus `Ref` (whose violation the fixture
    // shows inside `Guarded` only is still its own).
    let marked: BTreeSet<&str> =
        fixture.types.iter().filter(|t| t.invariant).map(|t| t.id.as_str()).collect();
    let violated: BTreeSet<&str> =
        fixture.vectors.iter().filter(|v| v.class == "invariant").map(|v| v.ty.as_str()).collect();
    assert!(violated.is_subset(&marked), "{:?}", violated.difference(&marked).collect::<Vec<_>>());
    assert_eq!(marked.len(), 16, "{marked:?}");
    for id in &marked {
        assert!(
            violated.contains(id) || fixture.vectors.iter().any(|v| v.ty == *id && v.class == "custom"),
            "{id}: marked, with no vector"
        );
    }
}
