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

/// Classes where C++ and Rust are known to differ by type design; see
/// docs/compatibility.md. The fixture records what C++ does; these are the
/// outcomes Rust must have instead.
fn rust_expectation(v: &Vector) -> &str {
    match v.class.as_str() {
        // C++ does not enforce a member length_bound; the Rust bounded types
        // do. Rust rejects what exceeds the bound.
        "length-bound-overflow" => "reject",
        // C++ accepts any underlying integer for an enum; a Rust enum can
        // only hold its enumerators.
        "enum-unknown-value" => "reject",
        // The same length_bound difference inside the `width` class: a
        // WidthCheckpoint filled to its 64-byte cap by a 58-byte label whose
        // member bound is 16. C++ checks only the cap.
        "width" if v.name.starts_with("width/cap/checkpoint_at_cap/") => "reject",
        _ => v.expect.as_str(),
    }
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
    // The documented divergences are confined to their two classes.
    let allowed = fixture
        .vectors
        .iter()
        .filter(|v| matches!(v.class.as_str(), "length-bound-overflow" | "enum-unknown-value"))
        .count();
    assert_eq!(divergent, allowed + 4, "C++/Rust divergences are exactly the documented ones");
}

#[test]
fn the_fixture_covers_every_required_class_in_every_profile() {
    let fixture = fixture();
    for class in [
        "corpus", "evolution-forward", "evolution-backward", "pruning", "malformed",
        "optional-presence", "width", "tuple", "variant", "enum", "enum-unknown-value",
        "string", "length-bound-overflow", "bytes", "zero-fixed", "depth",
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
        ("depth_3", native(DepthNode { value: 1, next: Some(Box::new(DepthNode { value: 7, next: None })) })),
    ]
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
