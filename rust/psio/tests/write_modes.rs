//! The four modes, as one property (C++ `value_contract_tests.cpp`, "the four
//! modes"; docs/annotations.md 7.2; gofractally/psio#14).
//!
//! For values drawn at random — most of them outside what their type
//! declares — and for pSSZ, keys and JSON:
//!
//! 1. the fast write (`to_F`), then the trusted read (`from_F_trusted`),
//!    gives the value back (unless the format cannot represent the value at
//!    all, and then the validated write refuses it too);
//! 2. the validated write (`try_to_F`) refuses exactly when the validating
//!    read (`from_F`) of the bytes the fast write produced refuses.
//!
//! So a service's own row, written fast and read trusted, is the row it
//! wrote, whatever it declares.

use psio::pssz::{BoundedList, BoundedString};
use std::collections::BTreeMap;

#[derive(Debug, Clone, PartialEq, Default, psio::PsszPack, psio::PsszUnpack, psio::JsonText, psio::Key)]
struct Item {
    #[psio(max = 3)]
    code: String,
    qty: u32,
}

#[derive(Debug, Clone, PartialEq, Default, psio::PsszPack, psio::PsszUnpack, psio::JsonText, psio::Key)]
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

#[derive(Debug, Clone, PartialEq, Default, psio::PsszPack, psio::PsszUnpack, psio::JsonText, psio::Key)]
struct Prop {
    #[psio(sorted, max = 3)]
    ids: Vec<u32>,
    #[psio(max = 6)]
    name: String,
    #[psio(max = 2, element(max = 3))]
    tags: Vec<String>,
    #[psio(max = 2)]
    items: Vec<Item>,
    even: Even,
    wrapped: BoundedString<4>,
    list: BoundedList<u16, 2>,
}

/// The same with a map (not a key type).
#[derive(Debug, Clone, PartialEq, Default, psio::PsszPack, psio::PsszUnpack, psio::JsonText)]
struct WithMap {
    #[psio(max = 1, keys(max = 2), values(max = 2))]
    attrs: BTreeMap<String, String>,
    #[psio(unique)]
    codes: Vec<u32>,
    prop: Prop,
}

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9e3779b97f4a7c15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb);
        z ^ (z >> 31)
    }
    fn below(&mut self, n: usize) -> usize {
        (self.next() % n as u64) as usize
    }
    fn text(&mut self, max: usize) -> String {
        (0..self.below(max + 1)).map(|_| (b'a' + self.below(26) as u8) as char).collect()
    }
}

fn prop(r: &mut Rng) -> Prop {
    let mut ids: Vec<u32> = (0..r.below(6)).map(|_| r.below(4) as u32).collect();
    if r.below(2) == 1 {
        ids.sort();
    }
    Prop {
        ids,
        name: r.text(9),
        tags: (0..r.below(4)).map(|_| r.text(5)).collect(),
        items: (0..r.below(4)).map(|_| Item { code: r.text(5), qty: r.below(100) as u32 }).collect(),
        even: Even { v: r.below(6) as u32 },
        wrapped: BoundedString(r.text(6)),
        list: BoundedList((0..r.below(4)).map(|_| r.below(9) as u16).collect()),
    }
}

fn with_map(r: &mut Rng) -> WithMap {
    WithMap {
        attrs: (0..r.below(3)).map(|_| (r.text(3), r.text(3))).collect(),
        codes: (0..r.below(4)).map(|_| r.below(5) as u32).collect(),
        prop: prop(r),
    }
}

#[derive(Default)]
struct Counts {
    valid: usize,
    invalid: usize,
    unwritable: usize,
}

/// One format: `fast` (None: the value has no encoding), `validated`,
/// `trusted`, `validating`.
fn four_modes<T: PartialEq + std::fmt::Debug, B: PartialEq + std::fmt::Debug + AsRef<[u8]>>(
    value: &T,
    fast: impl Fn(&T) -> B + std::panic::RefUnwindSafe,
    validated: impl Fn(&T) -> Option<B>,
    trusted: impl Fn(&[u8]) -> T,
    validating: impl Fn(&[u8]) -> bool,
    counts: &mut Counts,
) where
    T: std::panic::RefUnwindSafe,
{
    let written = validated(value);
    let Ok(bytes) = std::panic::catch_unwind(|| fast(value)) else {
        // No encoding: the format cannot represent the value; then the
        // validated write refuses it too.
        assert!(written.is_none(), "{value:?}");
        counts.unwritable += 1;
        return;
    };
    // 1. fast write -> trusted read.
    assert_eq!(&trusted(bytes.as_ref()), value);
    // 2. validated write <=> validating read of the fast bytes.
    assert_eq!(written.is_some(), validating(bytes.as_ref()), "{value:?}");
    match written {
        Some(same) => {
            assert_eq!(same, bytes);
            counts.valid += 1;
        }
        None => counts.invalid += 1,
    }
}

fn quiet<R>(body: impl FnOnce() -> R) -> R {
    // The fast pSSZ write panics for a value that has no frame; that is
    // expected here, and its message is noise.
    let hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {}));
    let result = body();
    std::panic::set_hook(hook);
    result
}

#[test]
fn fast_write_trusted_read_round_trip_and_the_two_validations_agree() {
    quiet(|| {
        let mut r = Rng(1);
        let (mut pssz, mut key, mut json, mut ijson) =
            (Counts::default(), Counts::default(), Counts::default(), Counts::default());
        for _ in 0..4000 {
            let value = prop(&mut r);
            four_modes(
                &value,
                |v| psio::to_pssz(v),
                |v| psio::try_to_pssz(v).ok(),
                |b| psio::from_pssz_trusted::<Prop>(b),
                |b| psio::from_pssz::<Prop>(b).is_ok() && psio::validate_pssz::<Prop>(b).is_ok(),
                &mut pssz,
            );
            four_modes(
                &value,
                |v| psio::to_key(v),
                |v| psio::try_to_key(v).ok(),
                |b| psio::from_key_trusted::<Prop>(b),
                |b| psio::from_key::<Prop>(b).is_ok() && psio::validate_key::<Prop>(b).is_ok(),
                &mut key,
            );
            four_modes(
                &value,
                |v| psio::to_json(v),
                |v| psio::try_to_json(v).ok(),
                |b| psio::from_json_trusted::<Prop>(b),
                |b| psio::from_json::<Prop>(b).is_ok() && psio::validate_json::<Prop>(b).is_ok(),
                &mut json,
            );
            four_modes(
                &value,
                |v| psio::to_ijson(v),
                |v| psio::try_to_ijson(v).ok(),
                |b| psio::from_ijson_trusted::<Prop>(b),
                |b| psio::from_ijson::<Prop>(b).is_ok(),
                &mut ijson,
            );
        }
        for (name, c) in [("pssz", &pssz), ("key", &key), ("json", &json), ("ijson", &ijson)] {
            println!("{name}: {} valid, {} invalid, {} without an encoding", c.valid, c.invalid, c.unwritable);
            assert!(c.valid > 5 && c.invalid > 2000, "{name}: the draw exercises both sides");
        }
        // Keys and JSON represent every value.
        assert_eq!((key.unwritable, json.unwritable, ijson.unwritable), (0, 0, 0));
    });
}

#[test]
fn the_same_with_maps_and_unique_lists() {
    quiet(|| {
        let mut r = Rng(2);
        let (mut pssz, mut json) = (Counts::default(), Counts::default());
        for _ in 0..3000 {
            let value = with_map(&mut r);
            four_modes(
                &value,
                |v| psio::to_pssz(v),
                |v| psio::try_to_pssz(v).ok(),
                |b| psio::from_pssz_trusted::<WithMap>(b),
                |b| psio::from_pssz::<WithMap>(b).is_ok(),
                &mut pssz,
            );
            four_modes(
                &value,
                |v| psio::to_json(v),
                |v| psio::try_to_json(v).ok(),
                |b| psio::from_json_trusted::<WithMap>(b),
                |b| psio::from_json::<WithMap>(b).is_ok(),
                &mut json,
            );
        }
        assert!(pssz.invalid > 1500 && json.invalid > 1500);
    });
}

#[test]
fn a_row_written_fast_is_read_back_trusted() {
    // What a service does with its own table.
    let row = Prop {
        ids: vec![3, 1, 1, 9, 9],          // not sorted, over max
        name: "much too long".into(),      // over max
        tags: vec!["abcdef".into()],       // an element over its bound
        items: vec![Item::default(); 3],   // over max
        even: Even { v: 3 },               // violates its invariant
        wrapped: BoundedString("toolong".into()),
        list: BoundedList(vec![1, 2, 3]),
    };
    assert!(psio::try_to_pssz(&row).is_err());
    let bytes = psio::to_pssz(&row);
    assert!(psio::from_pssz::<Prop>(&bytes).is_err());
    assert!(psio::validate_pssz::<Prop>(&bytes).is_err());
    assert_eq!(psio::from_pssz_trusted::<Prop>(&bytes), row);

    assert!(psio::try_to_key(&row).is_err());
    let key = psio::to_key(&row);
    assert!(psio::from_key::<Prop>(&key).is_err());
    assert_eq!(psio::from_key_trusted::<Prop>(&key), row);

    assert!(psio::try_to_json(&row).is_err());
    let json = psio::to_json(&row);
    assert!(psio::from_json::<Prop>(&json).is_err());
    assert_eq!(psio::from_json_trusted::<Prop>(&json), row);

    // The appending forms: `to_F_into` fast, `try_to_F_into` validated.
    let mut out = vec![7u8];
    assert!(psio::try_to_pssz_into(&row, &mut out).is_err());
    assert_eq!(out, [7]);
    psio::to_pssz_into(&row, &mut out).unwrap();
    assert_eq!(&out[1..], &bytes[..]);
    let mut out = vec![7u8];
    assert!(psio::try_to_key_into(&row, &mut out).is_err());
    psio::to_key_into(&row, &mut out).unwrap();
    assert_eq!(&out[1..], &key[..]);
    let mut text = String::from("x");
    assert!(psio::try_to_json_into(&row, &mut text).is_err());
    psio::to_json_into(&row, &mut text).unwrap();
    assert_eq!(&text[1..], json);
}

#[derive(Debug, Clone, PartialEq, Default, psio::PsszPack, psio::PsszUnpack)]
struct Pair {
    #[psio(max = 100)]
    a: String,
    #[psio(max = 100)]
    b: String,
}

#[test]
fn the_fast_pssz_write_refuses_only_what_has_no_frame() {
    // The offsets of `Pair` are one byte wide (the bounds add up to less
    // than 256).
    let last_long = Pair { a: "a".into(), b: "b".repeat(300) };
    // Far over its bound and still a frame: the last member ends where the
    // record ends, so no offset names a position past 255.
    assert!(psio::try_to_pssz(&last_long).is_err());
    let bytes = psio::to_pssz(&last_long);
    assert!(psio::validate_pssz::<Pair>(&bytes).is_err());
    assert_eq!(psio::from_pssz_trusted::<Pair>(&bytes), last_long);
    // An offset that does not fit: no frame. The fast write fails (it
    // panics: `to_pssz` is infallible), and never writes a frame that reads
    // as another value.
    let first_long = Pair { a: "a".repeat(300), b: "b".into() };
    assert!(psio::try_to_pssz(&first_long).is_err());
    let mut out = vec![1u8];
    assert!(psio::to_pssz_into(&first_long, &mut out).is_err());
    assert_eq!(out, [1]);
    assert!(quiet(|| std::panic::catch_unwind(|| psio::to_pssz(&first_long))).is_err());
}

/// An explicit validating read or validated write validates whatever the
/// caller is doing: a fast write or a trusted read in progress (the scope a
/// hand-written codec or a text handler runs in) does not leak into it.
#[test]
fn a_validating_call_inside_a_fast_write_or_a_trusted_read_still_validates() {
    let row = Prop { name: "much too long".into(), even: Even { v: 3 }, ..Prop::default() };
    let loose_pssz = psio::to_pssz(&row);
    let loose_key = psio::to_key(&row);
    let loose_json = psio::to_json(&row);
    let check = |what: &str| {
        assert!(psio::from_pssz::<Prop>(&loose_pssz).is_err(), "{what}: from_pssz");
        assert!(psio::validate_pssz::<Prop>(&loose_pssz).is_err(), "{what}: validate_pssz");
        assert!(psio::pssz::decode::<psio::Pssz, Prop>(&loose_pssz).is_err(), "{what}: pssz::decode");
        assert!(psio::try_to_pssz(&row).is_err(), "{what}: try_to_pssz");
        assert!(psio::pssz::try_encode::<psio::Pssz, _>(&row).is_err(), "{what}: pssz::try_encode");
        let mut out = Vec::new();
        assert!(psio::try_to_pssz_into(&row, &mut out).is_err(), "{what}: try_to_pssz_into");
        assert!(psio::from_key::<Prop>(&loose_key).is_err(), "{what}: from_key");
        assert!(psio::validate_key::<Prop>(&loose_key).is_err(), "{what}: validate_key");
        assert!(psio::try_to_key(&row).is_err(), "{what}: try_to_key");
        assert!(psio::try_to_key_into(&row, &mut out).is_err(), "{what}: try_to_key_into");
        assert!(psio::from_json::<Prop>(&loose_json).is_err(), "{what}: from_json");
        assert!(psio::validate_json::<Prop>(&loose_json).is_err(), "{what}: validate_json");
        assert!(psio::from_ijson::<Prop>(&psio::to_ijson(&row)).is_err(), "{what}: from_ijson");
        assert!(psio::try_to_json(&row).is_err(), "{what}: try_to_json");
        assert!(psio::try_to_ijson(&row).is_err(), "{what}: try_to_ijson");
        let mut text = String::new();
        assert!(psio::try_to_json_into(&row, &mut text).is_err(), "{what}: try_to_json_into");
        assert!(psio::pssz::Nested::<Prop>::new(&row).is_err(), "{what}: Nested::new");
    };
    check("outside any scope");
    {
        // What the body of a fast write or of a trusted read runs in.
        let _scope = psio::pssz_checked::Undeclared::enter();
        check("inside a fast write / trusted read");
        // ... and the enclosing mode is restored afterwards.
        assert!(!psio::pssz_checked::declared());
    }
    assert!(psio::pssz_checked::declared());
}

/// The same reached without naming the scope: a text handler that validates
/// other bytes while the value it belongs to is written fast.
#[derive(Debug, Clone, PartialEq, Default, psio::PsszPack, psio::PsszUnpack)]
struct Envelope {
    payload: Vec<u8>,
}
impl psio::json::JsonText for Envelope {
    fn write_json(&self, w: &mut psio::json::Writer) -> Result<(), psio::json::JsonTextError> {
        // The payload is the pSSZ of a Prop: this writer refuses to give text
        // to bytes that are not one.
        match psio::from_pssz::<Prop>(&self.payload) {
            Ok(prop) => prop.write_json(w),
            Err(_) => Err(w.err("Envelope: the payload is not a Prop")),
        }
    }
    fn read_json(r: &mut psio::json::Reader) -> Result<Self, psio::json::JsonTextError> {
        let prop = Prop::read_json(r)?;
        Ok(Envelope { payload: psio::to_pssz(&prop) })
    }
}

#[test]
fn a_handler_that_validates_is_not_switched_off_by_the_fast_write_around_it() {
    let bad = Prop { name: "much too long".into(), ..Prop::default() };
    let envelope = Envelope { payload: psio::to_pssz(&bad) };
    // The handler's own check is its own: it holds in both modes.
    assert!(psio::try_to_json(&envelope).is_err());
    let mut text = String::new();
    assert!(psio::to_json_into(&envelope, &mut text).is_err(), "the fast write runs the handler's validating read");
}
