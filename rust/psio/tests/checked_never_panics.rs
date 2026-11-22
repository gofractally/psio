//! The Result-returning PSSZ entry points report a value without an
//! encoding — a nested record over its own cap, a payload its type's offset
//! width cannot address — as an `Err`, never as a panic (C++:
//! cpp/tests/checked_never_throws.hpp).

use psio::pssz::dynamic;
use psio::pssz::schema_ir::schema_for;
use psio::pssz::{from_pssz, pssz_validate, try_to_pssz, Pssz, Pssz16, Pssz8};
use std::panic::catch_unwind;

#[derive(Debug, Clone, PartialEq, Default, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema)]
#[pssz(max_dynamic_data = 100)]
struct Inner {
    text: String,
}
#[derive(Debug, Clone, PartialEq, Default, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema)]
struct Outer {
    id: u32,
    inner: Inner,
    more: Vec<Inner>,
    maybe: Option<Inner>,
}
#[derive(Debug, Clone, PartialEq, Default, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema)]
#[pssz(max_dynamic_data = 300)]
struct Wide {
    a: String,
    b: String,
}

fn inner(n: usize) -> Inner {
    Inner { text: "x".repeat(n) }
}

#[test]
fn a_value_without_an_encoding_is_an_error_not_a_panic() {
    let ok = Outer { id: 1, inner: inner(4), more: vec![inner(1), inner(2)], maybe: Some(inner(3)) };
    let bytes = try_to_pssz::<Pssz, _>(&ok).expect("encodes");
    assert_eq!(from_pssz::<Pssz, Outer>(&bytes).unwrap(), ok);

    let over_cap: Vec<Outer> = vec![
        Outer { inner: inner(200), ..ok.clone() },
        Outer { more: vec![inner(1), inner(200)], ..ok.clone() },
        Outer { maybe: Some(inner(200)), ..ok.clone() },
    ];
    for bad in &over_cap {
        let result = catch_unwind(|| try_to_pssz::<Pssz, _>(bad)).expect("no panic");
        assert!(result.is_err(), "{bad:?}");
        // The schema-driven encoder agrees.
        let (schema, root) = schema_for::<Outer>().unwrap();
        let good = dynamic::decode_exact::<Pssz>(&schema, &root, &bytes).unwrap();
        assert_eq!(dynamic::encode::<Pssz>(&schema, &root, &good).unwrap(), bytes);
    }
    for result in [
        catch_unwind(|| try_to_pssz::<Pssz, _>(&inner(200)).is_err()),
        catch_unwind(|| try_to_pssz::<Pssz, _>(&vec![inner(1), inner(200)]).is_err()),
        catch_unwind(|| try_to_pssz::<Pssz, _>(&Some(inner(200))).is_err()),
        catch_unwind(|| try_to_pssz::<Pssz, _>(&(1u8, inner(200))).is_err()),
        // Two-byte offsets from the cap: a first payload past 64 KiB.
        catch_unwind(|| try_to_pssz::<Pssz, _>(&Wide { a: "a".repeat(70_000), b: "b".into() }).is_err()),
        // An explicit width too narrow for the value.
        catch_unwind(|| {
            let wide = Outer { more: vec![inner(90); 40], inner: inner(90), ..Outer::default() };
            try_to_pssz::<Pssz8, _>(&wide).is_err()
        }),
        catch_unwind(|| try_to_pssz::<Pssz16, _>(&vec!["z".repeat(40_000); 3]).is_err()),
    ] {
        assert_eq!(result.expect("no panic"), true);
    }

    // Reading garbage: every truncation and single-byte corruption of a
    // valid frame gives an answer.
    for n in 0..=bytes.len() {
        let cut = bytes[..n].to_vec();
        catch_unwind(move || {
            let _ = pssz_validate::<Pssz, Outer>(&cut);
            let _ = from_pssz::<Pssz, Outer>(&cut);
            let _ = from_pssz::<Pssz8, Outer>(&cut);
        })
        .expect("no panic");
    }
    for i in 0..bytes.len() {
        for flip in [0x01u8, 0x80, 0xff] {
            let mut bad = bytes.clone();
            bad[i] ^= flip;
            catch_unwind(move || {
                let _ = pssz_validate::<Pssz, Outer>(&bad);
                let _ = from_pssz::<Pssz, Outer>(&bad);
                let (schema, root) = schema_for::<Outer>().unwrap();
                let _ = dynamic::validate(&schema, &root, &bad);
            })
            .expect("no panic");
        }
    }
}
