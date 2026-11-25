//! `conformance/protobuf-vectors.json` (described in the header of
//! `cpp/tests/protobuf_conformance_emit.cpp`): protobuf messages written by
//! the C++ implementation — the bytes the official library writes for the
//! same values — messages of other writers, and bytes its reader refuses.
//!
//! * every `accept` row decodes, and what it decodes to encodes to the same
//!   bytes (and `protobuf_size` is their length);
//! * every `accept_other` row is accepted and re-encodes to `canonical_hex`;
//! * every `reject` row is refused, with the message and at the offset C++
//!   states;
//! * every digest is reproduced: the verdict, message and offset on 30,000
//!   corruptions of a row per type.

mod protobuf_shapes;

use protobuf_shapes::{corrupt, PbRng};
use serde_json::Value;

fn fixture() -> Value {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../conformance/protobuf-vectors.json");
    serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap()
}

fn unhex(s: &str) -> Vec<u8> {
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
}

fn text<'a>(row: &'a Value, key: &str) -> &'a str {
    row[key].as_str().unwrap()
}

#[test]
fn the_fixture_is_the_version_this_test_reads() {
    assert_eq!(fixture()["version"], 1);
}

#[test]
fn accepted_rows_decode_and_encode_to_the_same_bytes() {
    let fx = fixture();
    let rows = fx["accept"].as_array().unwrap();
    assert!(rows.len() >= 241);
    for row in rows {
        let (id, bytes) = (text(row, "id"), unhex(text(row, "wire_hex")));
        with_pb_type!(text(row, "type"), T => {
            psio::validate_protobuf::<T>(&bytes).unwrap_or_else(|e| panic!("{id}: {e}"));
            let value = psio::from_protobuf::<T>(&bytes).unwrap_or_else(|e| panic!("{id}: {e}"));
            assert_eq!(psio::to_protobuf(&value), bytes, "{id}");
            assert_eq!(psio::protobuf_size(&value), bytes.len(), "{id}");
            let mut sink = vec![0xaa];
            psio::to_protobuf_into(&value, &mut sink);
            assert_eq!(&sink[1..], &bytes[..], "{id}");
        });
    }
}

#[test]
fn rows_of_other_writers_are_accepted_and_normalised() {
    let fx = fixture();
    let rows = fx["accept_other"].as_array().unwrap();
    assert!(rows.len() >= 32);
    for row in rows {
        let (id, bytes) = (text(row, "id"), unhex(text(row, "wire_hex")));
        let canonical = unhex(text(row, "canonical_hex"));
        with_pb_type!(text(row, "type"), T => {
            psio::validate_protobuf::<T>(&bytes).unwrap_or_else(|e| panic!("{id}: {e}"));
            let value = psio::from_protobuf::<T>(&bytes).unwrap_or_else(|e| panic!("{id}: {e}"));
            assert_eq!(psio::to_protobuf(&value), canonical, "{id}");
        });
    }
}

#[test]
fn rejected_rows_are_refused_for_the_reason_and_where_cpp_states() {
    let fx = fixture();
    let rows = fx["reject"].as_array().unwrap();
    assert!(rows.len() >= 63);
    for row in rows {
        let (id, bytes) = (text(row, "id"), unhex(text(row, "wire_hex")));
        let expected = (text(row, "error"), row["at"].as_u64().unwrap() as u32);
        with_pb_type!(text(row, "type"), T => {
            let e = psio::validate_protobuf::<T>(&bytes).expect_err(id);
            assert_eq!((e.message(), e.offset()), expected, "{id}");
            let d = psio::from_protobuf::<T>(&bytes).expect_err(id);
            assert_eq!(d, e, "{id}");
        });
    }
}

#[test]
fn the_verdicts_on_corruptions_are_those_of_cpp() {
    let fx = fixture();
    let digests = fx["mutation_digests"].as_array().unwrap();
    assert_eq!(digests.len(), 13);
    let mut total = 0u64;
    let mut total_accepted = 0u64;
    for row in digests {
        let base_id = text(row, "base");
        let base = fx["accept"]
            .as_array()
            .unwrap()
            .iter()
            .find(|r| text(r, "id") == base_id)
            .map(|r| unhex(text(r, "wire_hex")))
            .unwrap();
        let count = row["count"].as_u64().unwrap();
        let mut rng = PbRng::new(row["seed"].as_u64().unwrap());
        let mut h = 0xcbf29ce484222325u64;
        let mut accepted = 0u64;
        let mut fold = |b: u8| {
            h ^= b as u64;
            h = h.wrapping_mul(0x100000001b3);
        };
        for _ in 0..count {
            let c = corrupt(&base, &mut rng);
            let verdict = with_pb_type!(text(row, "type"), T => {
                let v = psio::validate_protobuf::<T>(&c);
                // Validate and decode are one reader.
                assert_eq!(psio::from_protobuf::<T>(&c).err(), v.err());
                v
            });
            match verdict {
                Ok(()) => {
                    fold(1);
                    accepted += 1;
                }
                Err(e) => {
                    fold(0);
                    e.message().bytes().for_each(&mut fold);
                    e.offset().to_le_bytes().into_iter().for_each(&mut fold);
                }
            }
        }
        assert_eq!(accepted, row["accepted"].as_u64().unwrap(), "{base_id}: accepted");
        assert_eq!(format!("{h:016x}"), text(row, "digest"), "{base_id}: digest");
        total += count;
        total_accepted += accepted;
    }
    assert_eq!(total, 390_000);
    println!("{total} corruptions, {total_accepted} accepted: every verdict, message and offset as in C++");
}
