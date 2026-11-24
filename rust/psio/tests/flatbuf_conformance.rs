//! `conformance/flatbuf-vectors.json` (described in
//! `conformance/flatbuf-vectors.md`): FlatBuffers buffers written by the C++
//! implementation — the bytes the official library writes for the same
//! values — and buffers its verifier refuses, with the reason.
//!
//! * every accepted row decodes to the value it names (checked decode,
//!   trusted decode, view) and that value encodes to the same bytes;
//! * every rejected row is refused, for the reason and at the position C++
//!   states;
//! * every corrupted row C++ accepts is accepted, and what it decodes to
//!   re-encodes to a valid buffer.
//!
//! "The same answer as C++" is exact for the `raw` declarations of
//! `flatbuf_shapes` (strings of any bytes, enums of any value). For the
//! `natural` ones (`String`, a plain `enum`) the verifier is stricter in two
//! ways, which this test pins: a string that is not UTF-8 and an enum value
//! that is not an enumerator are refused.

mod flatbuf_shapes;

use flatbuf_shapes::{natural, raw};
use psio::flatbuf::FbOptions;
use serde_json::Value;

fn fixture() -> Value {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../conformance/flatbuf-vectors.json");
    serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap()
}

fn unhex(s: &str) -> Vec<u8> {
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
}

struct Row {
    id: String,
    ty: String,
    bytes: Vec<u8>,
    opts: FbOptions,
    error: Option<(String, u32)>,
}

fn rows(fx: &Value, section: &str) -> Vec<Row> {
    fx[section]
        .as_array()
        .unwrap()
        .iter()
        .map(|row| Row {
            id: row["id"].as_str().unwrap().into(),
            ty: row["type"].as_str().unwrap().into(),
            bytes: unhex(row["wire_hex"].as_str().unwrap()),
            opts: FbOptions {
                file_identifier: row.get("file_identifier").map(|id| id.as_str().unwrap().as_bytes().try_into().unwrap()),
                size_prefixed: row.get("size_prefixed").is_some(),
            },
            error: row.get("error").map(|e| (e.as_str().unwrap().into(), row["at"].as_u64().unwrap() as u32)),
        })
        .collect()
}

/// The two reasons only the Rust declarations with `String` and a plain
/// `enum` have.
const RUST_ONLY: &[&str] = &["flatbuf: string is not valid UTF-8", "flatbuf: unknown enum value"];

#[test]
fn accepted_buffers() {
    let fx = fixture();
    assert_eq!(fx["version"], 2);
    let rows = rows(&fx, "accept");
    for row in &rows {
        assert!(natural::check_accepted(&row.id, &row.bytes, &row.opts), "unclassified row {}", row.id);
        assert!(raw::check_accepted(&row.id, &row.bytes, &row.opts), "unclassified row {}", row.id);
    }
    println!("flatbuf-vectors.json: {} accepted rows decoded and re-encoded to the same bytes", rows.len());
    assert_eq!(rows.len(), 27);
}

#[test]
fn rejected_buffers() {
    let fx = fixture();
    let rows = rows(&fx, "reject");
    let mut rust_first = Vec::new();
    for row in &rows {
        let (message, at) = row.error.as_ref().unwrap();
        // The C++ types' own contents: the C++ verdict, reason and position.
        let e = raw::verdict(&row.ty, &row.bytes, &row.opts).expect_err(&row.id);
        assert_eq!((e.message(), e.offset()), (message.as_str(), *at), "{}", row.id);
        // `String` and a plain enum: refused too. The reason is the same
        // unless one of the two Rust-only checks comes first in the walk.
        let e = natural::verdict(&row.ty, &row.bytes, &row.opts).expect_err(&row.id);
        if e.message() != message {
            assert!(RUST_ONLY.contains(&e.message()), "{}: {e}", row.id);
            rust_first.push(row.id.as_str());
        }
    }
    println!(
        "flatbuf-vectors.json: {} rejected rows refused with the C++ reason and position; \
         with String / enum members a Rust-only check came first in {} of them: {rust_first:?}",
        rows.len(),
        rust_first.len()
    );
    assert_eq!(rows.len(), 427);
}

#[test]
fn corrupted_buffers_the_verifier_accepts() {
    let fx = fixture();
    let rows = rows(&fx, "accept_corrupt");
    let mut stricter = Vec::new();
    for row in &rows {
        raw::verdict(&row.ty, &row.bytes, &row.opts).unwrap_or_else(|e| panic!("{}: {e}", row.id));
        raw::stable(&row.id, &row.ty, &row.bytes);
        match natural::verdict(&row.ty, &row.bytes, &row.opts) {
            Ok(()) => natural::stable(&row.id, &row.ty, &row.bytes),
            Err(e) => {
                assert!(RUST_ONLY.contains(&e.message()), "{}: {e}", row.id);
                stricter.push((row.id.as_str(), e.message()));
            }
        }
    }
    println!(
        "flatbuf-vectors.json: {} corrupted rows accepted as in C++; {} of them refused with String / enum \
         members: {stricter:?}",
        rows.len(),
        stricter.len()
    );
    assert_eq!(rows.len(), 62);
    // The two rows written to show the difference.
    assert!(stricter.contains(&("Profile/name_not_utf8", RUST_ONLY[0])));
    assert!(stricter.contains(&("Order/color_out_of_range", RUST_ONLY[1])));
}
