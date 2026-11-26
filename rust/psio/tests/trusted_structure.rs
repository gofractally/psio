//! Trusted reads on bytes that are not a frame of the type
//! (gofractally/psio#15; docs/annotations.md 7.2 "On bytes that are not a
//! frame"; C++ `cpp/tests/trusted_structure_tests.cpp`).
//!
//! A trusted read holds nothing against what a type declares. It is still
//! handed bytes that may not be a frame of the type at all: a truncated row,
//! a row an incompatible version of the type wrote. Pinned here:
//!
//! * `from_pssz_trusted` panics — its contract: it returns `T` — with a
//!   message that names the type and what did not fit, never with the
//!   standard library's slice / index / arithmetic message;
//! * `try_from_pssz_trusted` returns the same failure as an error;
//! * no trusted read of any format panics with such a message, over a few
//!   hundred thousand mutated frames;
//! * the views fail the same way.
//!
//! One test function: it replaces the panic hook for the process.

use psio::pssz::{BoundedList, BoundedString};
use std::collections::{BTreeMap, BTreeSet};
use std::panic::{catch_unwind, AssertUnwindSafe};

#[derive(Debug, Clone, PartialEq, Default, psio::PsszPack, psio::PsszUnpack)]
#[psio(transparent)]
pub struct Memo(#[psio(max = 80)] String);
#[derive(Debug, Clone, PartialEq, Default, psio::PsszPack, psio::PsszUnpack)]
struct Note {
    id: u32,
    memo: Memo,
}
#[derive(Debug, Clone, PartialEq, Default, psio::PsszPack, psio::PsszUnpack)]
struct Wide {
    id: u32,
    a: String,
    b: Vec<u64>,
    c: Option<String>,
}
#[derive(Debug, Clone, PartialEq, Default, psio::PsszPack, psio::PsszUnpack)]
struct Old {
    id: u32,
    a: String,
}
#[derive(Debug, Clone, PartialEq, Default, psio::PsszPack, psio::PsszUnpack)]
struct Fixed {
    x: u64,
    y: u64,
    z: u32,
}

// One of everything a trusted reader reads.
#[derive(Debug, Clone, PartialEq, Default, psio::PsszPack, psio::PsszUnpack, psio::JsonText, psio::Key)]
struct Item {
    #[psio(max = 12)]
    product: String,
    qty: u32,
}
#[derive(Debug, Clone, PartialEq, Default, psio::PsszPack, psio::PsszUnpack, psio::JsonText, psio::Key)]
struct Point {
    x: u32,
    y: u16,
    on: bool,
}
#[derive(Debug, Clone, PartialEq, Default, psio::PsszPack, psio::PsszUnpack, psio::JsonText)]
enum Choice {
    #[default]
    Nothing,
    Number(u32),
    Text(String),
    At(Point),
}
#[derive(Debug, Clone, PartialEq, Default, psio::PsszPack, psio::PsszUnpack, psio::JsonText)]
struct Rich {
    id: u64,
    flag: bool,
    name: String,
    at: Point,
    first: Item,
    items: Vec<Item>,
    numbers: Vec<u32>,
    names: Vec<String>,
    points: [Point; 2],
    triple: (u32, String, Point),
    grid: Vec<Vec<u16>>,
    by_name: BTreeMap<String, u32>,
    tags: BTreeSet<i32>,
    choice: Choice,
    bounded: BoundedList<u16, 4>,
    short: BoundedString<8>,
    note: Option<String>,
    count: Option<u32>,
    tail: Option<Item>,
}
#[derive(Debug, Clone, PartialEq, Default, psio::PsszPack, psio::PsszUnpack, psio::JsonText, psio::Key)]
struct Keyed {
    id: u64,
    name: String,
    at: Point,
    items: Vec<Item>,
    flag: bool,
}

fn rich() -> Rich {
    Rich {
        id: 7,
        flag: true,
        name: "alpha".into(),
        at: Point { x: 3, y: 4, on: true },
        first: Item { product: "first".into(), qty: 11 },
        items: vec![Item { product: "x".into(), qty: 1 }, Item { product: "yy".into(), qty: 2 }],
        numbers: vec![10, 20, 30],
        names: vec!["one".into(), String::new(), "three".into()],
        points: [Point { x: 1, y: 2, on: false }, Point { x: 3, y: 4, on: true }],
        triple: (1, "two".into(), Point { x: 5, y: 6, on: false }),
        grid: vec![vec![1, 2], vec![], vec![3]],
        by_name: [("a".to_string(), 1), ("bb".to_string(), 2)].into_iter().collect(),
        tags: [-3, 3, 30].into_iter().collect(),
        choice: Choice::At(Point { x: 7, y: 8, on: true }),
        bounded: BoundedList(vec![1u16, 2]),
        short: BoundedString("short".to_string()),
        note: Some("note".into()),
        count: Some(77),
        tail: Some(Item { product: "tail".into(), qty: 9 }),
    }
}
fn keyed() -> Keyed {
    Keyed {
        id: 9,
        name: "name".into(),
        at: Point { x: 1, y: 2, on: true },
        items: vec![Item { product: "p".into(), qty: 1 }, Item { product: "qq".into(), qty: 2 }],
        flag: true,
    }
}

// The message of a panic, or None for a call that returned.
fn panic_of<R>(run: impl FnOnce() -> R) -> Option<String> {
    match catch_unwind(AssertUnwindSafe(run)) {
        Ok(_) => None,
        Err(payload) => Some(
            payload
                .downcast_ref::<String>()
                .cloned()
                .or_else(|| payload.downcast_ref::<&str>().map(|s| s.to_string()))
                .unwrap_or_else(|| "(a panic that is not a message)".to_string()),
        ),
    }
}

// What the standard library says when code indexes, slices or computes
// without looking: none of it may reach a caller of a trusted read.
const NOT_OURS: &[&str] = &[
    "out of range for slice",
    "index out of bounds",
    "slice index starts at",
    "attempt to ",
    "called `Option::unwrap()`",
    "called `Result::unwrap()`",
    "capacity overflow",
    "byte index",
    "is not a char boundary",
    "copy_from_slice",
    "(a panic that is not a message)",
];
fn ours(format: &str, bytes: &[u8], message: &str) {
    for marker in NOT_OURS {
        assert!(
            !message.contains(marker),
            "{format}: a trusted read panicked with a library message on {bytes:02x?}: {message}"
        );
    }
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
}

fn rounds() -> usize {
    // PSIO_TRUSTED_STRUCTURE_ROUNDS: random mutations per (format, type).
    std::env::var("PSIO_TRUSTED_STRUCTURE_ROUNDS").ok().and_then(|text| text.parse().ok()).unwrap_or(20_000)
}

/// Every truncation, every byte set to 00 / ff / +1 / -1, ff ff and
/// ff ff ff ff at every position, and `rounds()` random edits of `good`.
fn mutations(good: &[u8], seed: u64, mut probe: impl FnMut(&[u8])) -> usize {
    let mut tried = 0;
    let mut run = |bytes: &[u8]| {
        tried += 1;
        probe(bytes)
    };
    run(good);
    for n in 0..good.len() {
        run(&good[..n]);
    }
    for i in 0..good.len() {
        for value in [0, 0xff, good[i].wrapping_add(1), good[i].wrapping_sub(1)] {
            let mut bad = good.to_vec();
            bad[i] = value;
            run(&bad);
        }
    }
    for width in [2usize, 4] {
        for i in 0..good.len().saturating_sub(width - 1) {
            let mut bad = good.to_vec();
            bad[i..i + width].fill(0xff);
            run(&bad);
        }
    }
    let mut rng = Rng(seed);
    for _ in 0..rounds() {
        let mut bad = good.to_vec();
        for _ in 0..1 + rng.below(4) {
            if bad.is_empty() {
                break;
            }
            match rng.below(6) {
                0 => {
                    let i = rng.below(bad.len());
                    bad[i] = rng.next() as u8;
                }
                1 => {
                    let i = rng.below(bad.len());
                    bad[i] = rng.below(bad.len() + 2) as u8;
                }
                2 => {
                    // A plausible offset, little-endian, 1 / 2 / 4 bytes.
                    let w = 1usize << rng.below(3);
                    let v = rng.below(bad.len() * 2 + 2) as u32;
                    let i = rng.below(bad.len());
                    for k in 0..w {
                        if i + k < bad.len() {
                            bad[i + k] = (v >> (8 * k)) as u8;
                        }
                    }
                }
                3 => {
                    let i = rng.below(bad.len());
                    let n = (1 + rng.below(8)).min(bad.len() - i);
                    bad.drain(i..i + n);
                }
                4 => {
                    let i = rng.below(bad.len());
                    let n = (1 + rng.below(8)).min(bad.len() - i);
                    let piece = bad[i..i + n].to_vec();
                    let at = rng.below(bad.len() + 1);
                    bad.splice(at..at, piece);
                }
                _ => {
                    let n = rng.below(bad.len() + 1);
                    bad.truncate(n);
                }
            }
        }
        run(&bad);
    }
    tried
}

fn pssz_probe<T>(name: &str, good: &[u8], seed: u64) -> (usize, usize)
where
    T: std::fmt::Debug + PartialEq + psio::pssz::PsszPack<psio::Pssz> + psio::pssz::PsszUnpack<psio::Pssz>
        + psio::pssz::PsszValidate<psio::Pssz>,
{
    let mut failed = 0;
    let tried = mutations(good, seed, |bytes| {
        let valid = psio::validate_pssz::<T>(bytes).is_ok();
        let trusted = panic_of(|| psio::from_pssz_trusted::<T>(bytes));
        if let Some(message) = &trusted {
            failed += 1;
            ours(name, bytes, message);
            assert!(
                message.starts_with("from_pssz_trusted: the bytes are not "),
                "{name}: {message} on {bytes:02x?}"
            );
            assert!(!valid, "{name}: the trusted read refused bytes that validate: {message} on {bytes:02x?}");
        }
        // The fallible form never panics; it refuses at least what the
        // trusted read refuses, and nothing that validates.
        let tried = panic_of(|| psio::try_from_pssz_trusted::<T>(bytes));
        match tried {
            None => {}
            Some(message) => panic!("{name}: try_from_pssz_trusted panicked: {message} on {bytes:02x?}"),
        }
        let result = psio::try_from_pssz_trusted::<T>(bytes);
        if valid {
            assert!(result.is_ok(), "{name}: {result:?} on {bytes:02x?}");
            assert!(trusted.is_none());
        }
        if trusted.is_some() {
            assert!(result.is_err(), "{name}: the fallible form accepted what the trusted read refused: {bytes:02x?}");
        }
        // A view of the bytes fails or reads, with a message of its own.
        if let Some(message) = panic_of(|| {
            if let Ok(view) = psio::PsszView::<T>::new(bytes) {
                let _ = view.bytes().len();
            }
        }) {
            ours(name, bytes, &message);
        }
    });
    (tried, failed)
}

#[test]
fn trusted_reads_of_bytes_that_are_not_a_frame() {
    // The probes panic by the hundred thousand: the hook is silent while
    // they run, and a failure of the test itself is reported at the end.
    let hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {}));
    let outcome = panic_of(probes);
    std::panic::set_hook(hook);
    if let Some(message) = outcome {
        panic!("{message}");
    }
}

fn probes() {

    // ── The cases of the report ─────────────────────────────────────────────
    let note = psio::to_pssz(&Note { id: 7, memo: Memo("abcdefgh".into()) });
    let wide = psio::to_pssz(&Wide { id: 7, a: "hello".into(), b: vec![1, 2, 3], c: Some("opt".into()) });
    let fixed = psio::to_pssz(&Fixed { x: 1, y: 2, z: 3 });
    let edited = |bytes: &[u8], edits: &[(usize, u8)]| {
        let mut out = bytes.to_vec();
        for &(at, value) in edits {
            out[at] = value;
        }
        out
    };
    fn case<T>(name: &str, bytes: Vec<u8>, says: &str)
    where
        T: std::fmt::Debug + psio::pssz::PsszPack<psio::Pssz> + psio::pssz::PsszUnpack<psio::Pssz>
            + psio::pssz::PsszValidate<psio::Pssz>,
    {
        assert!(psio::validate_pssz::<T>(&bytes).is_err(), "{name}");
        let message = panic_of(|| psio::from_pssz_trusted::<T>(&bytes)).unwrap_or_else(|| panic!("{name}: no failure"));
        ours(name, &bytes, &message);
        assert!(message.contains(says), "{name}: {message}");
        assert!(psio::try_from_pssz_trusted::<T>(&bytes).is_err(), "{name}");
    }
    let frame = |ty: &str| format!("from_pssz_trusted: the bytes are not a frame of {ty}: ");
    let wide_frame = frame(std::any::type_name::<Wide>());
    let note_frame = frame(std::any::type_name::<Note>());
    case::<Note>("Note: no bytes", vec![], &(note_frame.clone() + "the header needs bytes 0..1 of 0"));
    case::<Note>("Note: cut in the fixed part", note[..3].to_vec(), &note_frame);
    case::<Note>("Note: cut before its offset", note[..5].to_vec(), &note_frame);
    case::<Note>("Note: an offset past the end", edited(&note, &[(5, 200)]), &(note_frame.clone() + "a member needs bytes 201..14 of 14"));
    case::<Note>("Note: a header larger than the buffer", edited(&note, &[(0, 200)]), &note_frame);
    case::<Wide>("Wide: cut to 10 bytes", wide[..10].to_vec(), &(wide_frame.clone() + "the offset of a member needs bytes 8..12 of 10"));
    case::<Wide>("Wide: the first offset 0xffff", edited(&wide, &[(8, 0xff), (9, 0xff)]), &(wide_frame.clone() + "a member needs bytes 65539..25 of 53"));
    case::<Wide>("Wide: offsets out of order", edited(&wide, &[(12, 5)]), &(wide_frame.clone() + "a member needs bytes "));
    case::<Wide>("Wide: the payload cut", wide[..wide.len() - 9].to_vec(), &wide_frame);
    case::<Wide>("Wide: a header larger than the buffer", edited(&wide, &[(0, 0xff)]), &wide_frame);
    case::<Wide>(
        "Wide: a row of an older type",
        psio::to_pssz(&Old { id: 7, a: "hello".into() }),
        &(wide_frame.clone() + "a required member the frame does not have"),
    );
    case::<Fixed>("Fixed: cut", fixed[..10].to_vec(), &frame(std::any::type_name::<Fixed>()));
    // Not structure, but what a `String` needs: said as such.
    case::<Wide>("Wide: a is not UTF-8", edited(&wide, &[(20, 0xff)]), "from_pssz_trusted: the bytes are not valid: pssz: string is not UTF-8");

    // A value the fast write wrote outside what its type declares is a
    // frame: both trusted reads give it back.
    let over = Note { id: 1, memo: Memo("m".repeat(100)) };
    let bytes = psio::to_pssz(&over);
    assert!(psio::from_pssz::<Note>(&bytes).is_err());
    assert_eq!(psio::from_pssz_trusted::<Note>(&bytes), over);
    assert_eq!(psio::try_from_pssz_trusted::<Note>(&bytes).unwrap(), over);

    // ── Every mutation, every format ────────────────────────────────────────
    let mut total = 0;
    let (tried, failed) = pssz_probe::<Rich>("pssz Rich", &psio::to_pssz(&rich()), 1);
    assert!(failed > tried / 10, "pssz Rich: {failed} of {tried}");
    total += tried;
    let (tried, failed) = pssz_probe::<Wide>("pssz Wide", &wide, 2);
    assert!(failed > tried / 10, "pssz Wide: {failed} of {tried}");
    total += tried;
    total += pssz_probe::<Keyed>("pssz Keyed", &psio::to_pssz(&keyed()), 3).0;
    total += pssz_probe::<(u32, Vec<String>, Option<Vec<u8>>)>(
        "pssz tuple",
        &psio::to_pssz(&(5u32, vec!["a".to_string(), "bb".to_string()], Some(vec![1u8, 2, 3]))),
        4,
    )
    .0;

    // key and JSON: their trusted reads parse as strictly as ever.
    total += mutations(&psio::to_key(&keyed()), 5, |bytes| {
        if let Some(message) = panic_of(|| psio::key::from_key_trusted::<Keyed>(bytes)) {
            ours("key", bytes, &message);
            assert!(message.starts_with("from_key_trusted: the bytes are not valid: "), "key: {message}");
        }
    });
    total += mutations(psio::to_json(&rich()).as_bytes(), 6, |bytes| {
        if let Some(message) = panic_of(|| psio::json::from_json_trusted::<Rich>(bytes)) {
            ours("json", bytes, &message);
            assert!(message.starts_with("from_json_trusted: the text is not valid: "), "json: {message}");
        }
    });
    total += foreign::probe();
    eprintln!("trusted_structure: {total} mutated frames");
    assert!(total > 100_000 || rounds() < 20_000);
}

#[cfg(not(all(feature = "flatbuf", feature = "capnp", feature = "protobuf", feature = "wit", feature = "ssz")))]
mod foreign {
    pub fn probe() -> usize {
        0
    }
}

#[cfg(all(feature = "flatbuf", feature = "capnp", feature = "protobuf", feature = "wit", feature = "ssz"))]
mod foreign {
    use super::{mutations, ours, panic_of};

    #[derive(Debug, Clone, PartialEq, Default)]
    #[derive(psio::FbPack, psio::FbUnpack, psio::CapnpPack, psio::CapnpUnpack, psio::Protobuf)]
    #[derive(psio::WitPack, psio::WitUnpack)]
    pub struct Part {
        pub name: String,
        pub qty: u32,
    }
    #[derive(Debug, Clone, PartialEq, Default)]
    #[derive(psio::FbPack, psio::FbUnpack, psio::CapnpPack, psio::CapnpUnpack, psio::Protobuf)]
    #[derive(psio::WitPack, psio::WitUnpack)]
    pub struct Message {
        pub id: u64,
        pub name: String,
        pub flag: bool,
        pub parts: Vec<Part>,
        pub numbers: Vec<u32>,
        pub names: Vec<String>,
        pub one: Part,
    }
    fn message() -> Message {
        Message {
            id: 7,
            name: "alpha".into(),
            flag: true,
            parts: vec![Part { name: "x".into(), qty: 1 }, Part { name: "yy".into(), qty: 2 }],
            numbers: vec![10, 20, 30],
            names: vec!["one".into(), String::new(), "three".into()],
            one: Part { name: "one".into(), qty: 9 },
        }
    }

    pub fn probe() -> usize {
        let mut total = 0;
        total += mutations(&psio::to_flatbuf(&message()), 11, |bytes| {
            if let Some(m) = panic_of(|| psio::from_flatbuf_trusted::<Message>(bytes)) {
                ours("flatbuf", bytes, &m);
            }
        });
        // (Cap'n Proto is not walked: see
        // `capnp_reads_a_list_of_another_element_size` below.)
        total += mutations(&psio::to_wit(&message()), 13, |bytes| {
            if let Some(m) = panic_of(|| psio::from_wit_trusted::<Message>(bytes)) {
                ours("wit", bytes, &m);
            }
        });
        total += mutations(&psio::to_protobuf(&message()), 14, |bytes| {
            // (protobuf has no trusted read of its own: the validating one
            // is what a caller has.)
            if let Some(m) = panic_of(|| psio::from_protobuf::<Message>(bytes)) {
                panic!("protobuf: the validating read panicked: {m} on {bytes:02x?}");
            }
        });
        // ssz: a validating read of `bool`s must not build a `bool` from a
        // byte that is not 0 or 1 (the memcpy fast paths).
        assert!(psio::from_ssz::<Vec<bool>>(&[1, 0, 2]).is_err());
        assert!(psio::from_ssz::<[bool; 3]>(&[1, 0, 2]).is_err());
        assert_eq!(psio::from_ssz::<Vec<bool>>(&[1, 0, 1]).unwrap(), vec![true, false, true]);
        total += mutations(&psio::to_ssz(&vec![1u32, 2, 3]), 15, |bytes| {
            if let Some(m) = panic_of(|| psio::from_ssz_trusted::<Vec<u32>>(bytes)) {
                ours("ssz", bytes, &m);
            }
        });
        total
    }
}

/// OPEN (gofractally/psio#15): the Cap'n Proto reader validates the
/// structure of a message without the type, then reads it with the type;
/// the typed read walks a list by the element size and count the type
/// expects. On a message whose list pointer says otherwise `from_capnp` —
/// the VALIDATING read — panics with a slice message (these bytes), or
/// walks a count the bytes do not back (other mutations: gigabytes).
/// No `unsafe` is involved; it is not the contract of a validating read.
/// The fuzz above therefore does not walk capnp. Run with `--ignored` to see
/// it; when the reader is fixed, drop the `ignore` and add capnp to
/// `foreign::probe`.
#[cfg(feature = "capnp")]
#[test]
#[ignore = "open: the Rust capnp typed read is not bounded by the message (gofractally/psio#15)"]
fn capnp_reads_a_list_of_another_element_size() {
    #[derive(Debug, Clone, PartialEq, Default, psio::CapnpPack, psio::CapnpUnpack)]
    pub struct Part {
        pub name: String,
        pub qty: u32,
    }
    #[derive(Debug, Clone, PartialEq, Default, psio::CapnpPack, psio::CapnpUnpack)]
    pub struct Message {
        pub id: u64,
        pub name: String,
        pub flag: bool,
        pub parts: Vec<Part>,
        pub numbers: Vec<u32>,
        pub names: Vec<String>,
        pub one: Part,
    }
    #[rustfmt::skip]
    const BYTES: &[u8] = &[
        0x00, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x05, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x27, 0x00,
        0x00, 0x00, 0x29, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x2d, 0x00, 0x00, 0x00, 0x1e, 0x00,
        0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x61, 0x6c, 0x70, 0x68, 0x61, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x79, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x14, 0x00,
        0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x22, 0x00,
        0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x32, 0x00,
        0x00, 0x00, 0x6f, 0x6e, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x74, 0x68, 0x72, 0x65, 0x65, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x22, 0x00, 0x00, 0x00, 0x6f, 0x6e, 0x65, 0x00, 0x00, 0x00,
        0x00, 0x00,
    ];
    assert!(psio::from_capnp::<Message>(BYTES).is_err());
}
