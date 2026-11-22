//! `from_pssz_trusted`: the reader of bytes the caller vouches for.
//!
//! It gives the checked reader's value for valid bytes (also asserted for
//! every canonical vector of conformance/pssz-vectors.json, by the
//! conformance harness), skips the checks that are not about memory safety,
//! and on bytes nobody should have vouched for panics or returns a value:
//! it is a safe function.

use psio::pssz::{BoundedList, BoundedString, RawString};
use psio::pssz::{PsszPack, PsszUnpack};
use std::collections::BTreeMap;

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
struct Line {
    sku: u64,
    quantity: u32,
    note: Option<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, PsszPack, PsszUnpack)]
#[psio(definition_will_not_change)]
struct Fixed {
    a: u64,
    b: u32,
    c: bool,
    d: [u8; 3],
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
enum Choice {
    None,
    Number(u32),
    Text(String),
}

fn short(memo: &Memo) -> Result<(), &'static str> {
    if memo.0.len() <= 4 { Ok(()) } else { Err("memo is too long") }
}
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
#[psio(transparent, invariant = "short")]
struct Memo(String);

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
#[pssz(max_dynamic_data = 4096)]
struct Order {
    id: u64,
    customer: String,
    raw: RawString,
    lines: Vec<Line>,
    fixed: Vec<Fixed>,
    #[psio(max = 4, element(max = 8))]
    tags: Vec<String>,
    bounded: BoundedList<BoundedString<8>, 4>,
    #[psio(at(0, max = 4))]
    pair: (String, u8),
    counts: BTreeMap<String, u32>,
    choice: Choice,
    nested: Option<Vec<Option<u16>>>,
    memo: Memo,
    when: psio::time::TimePoint<psio::time::Micros>,
    tail: Option<String>,
}

fn order() -> Order {
    Order {
        id: 7,
        customer: "alice".into(),
        raw: RawString(vec![0xff, 0x00]),
        lines: vec![
            Line { sku: 1, quantity: 2, note: None },
            Line { sku: 3, quantity: 4, note: Some("gift".into()) },
        ],
        fixed: vec![Fixed { a: 1, b: 2, c: true, d: [1, 2, 3] }, Fixed { a: 4, b: 5, c: false, d: [0; 3] }],
        tags: vec!["new".into(), "x".into()],
        bounded: BoundedList(vec![BoundedString("b".into())]),
        pair: ("ab".into(), 9),
        counts: [("a".to_string(), 1), ("b".to_string(), 2)].into(),
        choice: Choice::Text("t".into()),
        nested: Some(vec![Some(1), None]),
        memo: Memo("memo".into()),
        when: psio::time::TimePoint::from_ticks(1_795_342_530_123_456),
        tail: None,
    }
}

#[test]
fn valid_bytes_read_as_the_checked_reader_reads_them() {
    let value = order();
    let bytes = psio::to_pssz(&value);
    assert_eq!(psio::from_pssz_trusted::<Order>(&bytes), value);
    assert_eq!(psio::from_pssz_trusted::<Order>(&bytes), psio::from_pssz::<Order>(&bytes).unwrap());
    let many = vec![value.clone(), Order { tail: Some("t".into()), choice: Choice::None, ..value.clone() }];
    assert_eq!(psio::from_pssz_trusted::<Vec<Order>>(&psio::to_pssz(&many)), many);
    // Every profile.
    for_profile::<psio::pssz::Pssz8>(&Line { sku: 1, quantity: 2, note: Some("n".into()) });
    for_profile::<psio::pssz::Pssz16>(&value);
    for_profile::<psio::pssz::Pssz32>(&value);
    // Scalars, strings, lists and optionals at the root.
    assert_eq!(psio::from_pssz_trusted::<u64>(&psio::to_pssz(&9u64)), 9);
    assert_eq!(psio::from_pssz_trusted::<String>(&psio::to_pssz("text")), "text");
    assert_eq!(psio::from_pssz_trusted::<Vec<u32>>(&psio::to_pssz(&vec![1u32, 2, 3])), [1, 2, 3]);
    assert_eq!(psio::from_pssz_trusted::<Vec<String>>(&psio::to_pssz(&Vec::<String>::new())), Vec::<String>::new());
    assert_eq!(psio::from_pssz_trusted::<Option<String>>(&psio::to_pssz(&Some("s".to_string()))), Some("s".into()));
    assert_eq!(psio::from_pssz_trusted::<Vec<()>>(&psio::to_pssz(&Vec::<()>::new())), Vec::<()>::new());

    // A newer writer's unknown trailing field is skipped, as by from_pssz.
    #[derive(PsszPack)]
    struct Line2 {
        sku: u64,
        quantity: u32,
        note: Option<String>,
        later: Option<u8>,
    }
    let newer = psio::to_pssz(&Line2 { sku: 1, quantity: 2, note: None, later: Some(3) });
    assert_eq!(psio::from_pssz_trusted::<Line>(&newer), Line { sku: 1, quantity: 2, note: None });
}

fn for_profile<F: psio::pssz::PsszFormat>(value: &(impl PsszPack<F> + PsszUnpack<F> + PartialEq + std::fmt::Debug)) {
    fn same<F: psio::pssz::PsszFormat, T: PsszPack<F> + PsszUnpack<F> + PartialEq + std::fmt::Debug>(value: &T) {
        let bytes = psio::pssz::encode::<F, T>(value);
        assert_eq!(&psio::pssz::decode_trusted::<F, T>(&bytes), value);
    }
    same::<F, _>(value);
}

/// What trusted means: the caller vouches for the bytes, so the checks that
/// only guard against bytes nobody vouched for are not made.
#[test]
fn the_checks_that_are_not_about_memory_safety_are_skipped() {
    // A bound: a frame with 5 tags, which the checked reader refuses.
    #[derive(Debug, PartialEq, PsszPack, PsszUnpack)]
    struct Tags {
        #[psio(max = 4, element(max = 8))]
        tags: Vec<String>,
    }
    #[derive(PsszPack)]
    struct Wide {
        tags: Vec<String>,
    }
    let over = psio::pssz::encode::<psio::pssz::Pssz8, Wide>(&Wide { tags: vec!["t".into(); 5] });
    assert!(psio::pssz::decode::<psio::pssz::Pssz8, Tags>(&over).is_err());
    assert_eq!(psio::pssz::decode_trusted::<psio::pssz::Pssz8, Tags>(&over).tags.len(), 5);

    // An invariant.
    let long = psio::to_pssz("too long");
    assert!(psio::from_pssz::<Memo>(&long).is_err());
    assert_eq!(psio::from_pssz_trusted::<Memo>(&long).0, "too long");

    // A bool that is not 0 or 1 is read as true (a `bool` is still 0 or 1).
    assert!(psio::from_pssz::<bool>(&[2]).is_err());
    assert!(psio::from_pssz_trusted::<bool>(&[2]));
}

/// Memory safety is not skipped: bytes that are not valid panic or give a
/// value, whatever they are.
#[test]
fn invalid_bytes_panic_or_give_a_value() {
    // A string that is not UTF-8 is never a `String`.
    let not_utf8 = [0xffu8, 0xfe];
    assert!(std::panic::catch_unwind(|| psio::from_pssz_trusted::<String>(&not_utf8)).is_err());
    // A truncated scalar and a list offset beyond the frame.
    assert!(std::panic::catch_unwind(|| psio::from_pssz_trusted::<u64>(&[1, 2, 3])).is_err());
    let mut list = psio::to_pssz(&vec!["a".to_string(), "b".to_string()]);
    list[4] = 0xff;
    assert!(std::panic::catch_unwind(move || psio::from_pssz_trusted::<Vec<String>>(&list)).is_err());

    // Every prefix, every single-byte corruption and random bytes: the
    // outcome is a panic or a value (which is dropped normally).
    let previous = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {}));
    let valid = psio::to_pssz(&order());
    let mut outcomes = [0usize; 2];
    let mut attempt = |bytes: Vec<u8>| {
        let panicked = std::panic::catch_unwind(move || {
            let value = psio::from_pssz_trusted::<Order>(&bytes);
            // The value is usable: formatting walks every string and list.
            let _ = format!("{value:?}");
        })
        .is_err();
        outcomes[panicked as usize] += 1;
    };
    for end in 0..valid.len() {
        attempt(valid[..end].to_vec());
    }
    let mut state = 0x9e37_79b9_7f4a_7c15u64;
    let mut next = || {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state
    };
    for _ in 0..4000 {
        let mut bytes = valid.clone();
        for _ in 0..1 + next() % 3 {
            let at = (next() % bytes.len() as u64) as usize;
            bytes[at] = next() as u8;
        }
        attempt(bytes);
    }
    for _ in 0..2000 {
        let length = (next() % 96) as usize;
        attempt((0..length).map(|_| next() as u8).collect());
    }
    std::panic::set_hook(previous);
    // Both outcomes occur; neither is undefined behaviour.
    assert!(outcomes[0] > 0 && outcomes[1] > 0, "{outcomes:?}");
}
