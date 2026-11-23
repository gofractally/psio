//! `conformance/flatbuf-vectors.json` (described in
//! `conformance/flatbuf-vectors.md`): FlatBuffers buffers written by the C++
//! implementation — the bytes the official library writes for the same
//! values — and buffers a verifier must refuse.
//!
//! The Rust derive covers tables of scalars, strings, vectors and nested
//! tables. Every accepted row of those shapes is decoded to its value and
//! re-encoded to the same bytes here; the rows of other shapes (structs,
//! unions, optionals, sorted vectors, defaults that are not zero) are
//! counted, not read. Rejected rows: see `rejected_buffers`.

use psio::flatbuf::{FbPack, FbUnpack};
use serde_json::Value;

#[derive(Debug, Clone, PartialEq, Default, psio::FbPack, psio::FbUnpack)]
struct Point {
    x: i32,
    y: i32,
}

#[derive(Debug, Clone, PartialEq, Default, psio::FbPack, psio::FbUnpack)]
struct Profile {
    id: u64,
    name: String,
    email: String,
    age: u32,
    verified: bool,
}

#[derive(Debug, Clone, PartialEq, Default, psio::FbPack, psio::FbUnpack)]
struct Numbers {
    b: bool,
    i8: i8,
    u8: u8,
    i16: i16,
    u16: u16,
    i32: i32,
    u32: u32,
    i64: i64,
    u64: u64,
    f32: f32,
    f64: f64,
}

#[derive(Debug, Clone, PartialEq, Default, psio::FbPack, psio::FbUnpack)]
struct Tree {
    value: i32,
    name: String,
    children: Vec<Tree>,
}

#[derive(Debug, Clone, PartialEq, Default, psio::FbPack, psio::FbUnpack)]
struct Entry {
    name: String,
    count: u32,
    price: f64,
}

#[derive(Debug, Clone, PartialEq, Default, psio::FbPack, psio::FbUnpack)]
struct Basket {
    id: u64,
    owner: Profile,
    entries: Vec<Entry>,
    tags: Vec<String>,
    codes: Vec<u16>,
    bytes: Vec<u8>,
    total: f64,
    paid: bool,
}

fn sample_profile() -> Profile {
    Profile {
        id: 42,
        name: "Alice Example".into(),
        email: "alice@example.com".into(),
        age: 37,
        verified: true,
    }
}

fn leaf(value: i32, name: &str) -> Tree {
    Tree { value, name: name.into(), children: vec![] }
}

fn tree() -> Tree {
    Tree {
        value: 1,
        name: "root".into(),
        children: vec![
            leaf(2, ""),
            Tree { value: 0, name: "middle".into(), children: vec![leaf(0, ""), leaf(-7, "")] },
            leaf(2, ""),
        ],
    }
}

fn basket() -> Basket {
    Basket {
        id: 9_000_000_000,
        owner: sample_profile(),
        entries: vec![
            Entry { name: "bolt".into(), count: 12, price: 0.25 },
            Entry::default(),
            Entry { name: "washer".into(), count: 1, price: 1.5 },
        ],
        tags: vec!["a".into(), "".into(), "third".into()],
        codes: vec![1, 256, 65535],
        bytes: vec![0, 1, 254, 255, 7],
        total: 4.5,
        paid: true,
    }
}

fn numbers_extremes() -> Numbers {
    Numbers {
        b: true,
        i8: i8::MIN,
        u8: u8::MAX,
        i16: i16::MIN,
        u16: u16::MAX,
        i32: i32::MIN,
        u32: u32::MAX,
        i64: i64::MIN,
        u64: u64::MAX,
        f32: -1.5,
        f64: 1e300,
    }
}

fn fixture() -> Value {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../conformance/flatbuf-vectors.json");
    serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap()
}

fn unhex(s: &str) -> Vec<u8> {
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
}

/// Decodes `bytes` to `value`; when `same_bytes`, also encodes `value` to `bytes`.
fn check<T: FbPack + FbUnpack + PartialEq + std::fmt::Debug>(id: &str, bytes: &[u8], value: &T, same_bytes: bool) {
    assert_eq!(&T::fb_unpack(bytes).unwrap(), value, "{id}: decode");
    if same_bytes {
        assert_eq!(value.fb_pack(), bytes, "{id}: encode");
    }
}

/// The shapes the Rust derive cannot declare.
const NOT_IN_RUST: &[&str] = &["Order", "Defaults", "Renumbered", "WithDeprecated"];

#[test]
fn accepted_buffers() {
    let fx = fixture();
    let (mut read, mut skipped) = (0, 0);
    for row in fx["accept"].as_array().unwrap() {
        let id = row["id"].as_str().unwrap();
        let ty = row["type"].as_str().unwrap();
        let mut bytes = unhex(row["wire_hex"].as_str().unwrap());
        // A size prefix frames the buffer; a file identifier sits after the
        // root offset. The Rust builder writes neither: such rows are decoded
        // and not re-encoded.
        let plain = row.get("size_prefixed").is_none() && row.get("file_identifier").is_none();
        if row.get("size_prefixed").is_some() {
            let n = u32::from_le_bytes(bytes[..4].try_into().unwrap()) as usize;
            assert_eq!(n, bytes.len() - 4, "{id}: size prefix");
            bytes.drain(..4);
        }
        if NOT_IN_RUST.contains(&ty) {
            skipped += 1;
            continue;
        }
        match id {
            "Point/zero" => check(id, &bytes, &Point::default(), plain),
            "Point/values" => check(id, &bytes, &Point { x: 1, y: -2 }, plain),
            "Profile/empty" => check(id, &bytes, &Profile::default(), plain),
            "Profile/sample" | "Profile/identifier" | "Profile/size_prefixed" => {
                check(id, &bytes, &sample_profile(), plain)
            }
            "Numbers/zero" => check(id, &bytes, &Numbers::default(), plain),
            "Numbers/extremes" => check(id, &bytes, &numbers_extremes(), plain),
            "Tree/empty" => check(id, &bytes, &Tree::default(), plain),
            "Tree/nested" => check(id, &bytes, &tree(), plain),
            "Basket/empty" => check(id, &bytes, &Basket::default(), plain),
            "Basket/full" => check(id, &bytes, &basket(), plain),
            other => panic!("unclassified row {other}"),
        }
        read += 1;
    }
    println!("flatbuf-vectors.json: {read} rows read, {skipped} of shapes Rust cannot declare");
    assert_eq!(read, 12);
}

/// The Rust crate has no FlatBuffers verifier yet (C++ has
/// `validate_flatbuf`): `fb_unpack` checks only what it happens to index, so
/// an invalid buffer is refused with an `Err`, stops with a slice-index
/// panic, or — where nothing it reads is out of range — is accepted. It
/// never reads outside the buffer (safe Rust cannot). This test pins which
/// rejected rows Rust accepts today, so that the verifier, when it is
/// written, changes the list visibly; the goal is an empty list and no
/// panics.
#[test]
fn rejected_buffers() {
    let fx = fixture();
    let mut accepted = Vec::new();
    let mut refused = 0;
    for row in fx["reject"].as_array().unwrap() {
        let id = row["id"].as_str().unwrap().to_string();
        let ty = row["type"].as_str().unwrap();
        let bytes = unhex(row["wire_hex"].as_str().unwrap());
        if NOT_IN_RUST.contains(&ty) {
            continue;
        }
        let hook = std::panic::take_hook();
        std::panic::set_hook(Box::new(|_| {}));
        let ok = std::panic::catch_unwind(|| match ty {
            "Profile" => Profile::fb_unpack(&bytes).is_ok(),
            "Basket" => Basket::fb_unpack(&bytes).is_ok(),
            "Tree" => Tree::fb_unpack(&bytes).is_ok(),
            other => panic!("unclassified type {other}"),
        })
        .unwrap_or(false);
        std::panic::set_hook(hook);
        if ok {
            accepted.push(id);
        } else {
            refused += 1;
        }
    }
    println!("flatbuf-vectors.json rejects: {refused} refused, accepted for lack of a check: {accepted:?}");
    assert_eq!(accepted, EXPECTED_ACCEPTED_BY_RUST, "the set of invalid buffers Rust accepts changed");
}

const EXPECTED_ACCEPTED_BY_RUST: &[&str] = &[
    "Profile/null_root",
    "Profile/misaligned_table",
    "Profile/vtable_size_odd",
    "Profile/string_not_terminated",
    "Tree/too_deep",
];
