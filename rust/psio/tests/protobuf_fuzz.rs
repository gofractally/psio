//! `psio::protobuf` on bytes that are not what a writer produced: every
//! truncation and every byte of the fixture's accepted rows replaced,
//! random corruptions, random bytes. Nothing panics, validate and decode
//! give the same verdict, and what is accepted re-encodes to a fixed point.

mod protobuf_shapes;

use protobuf_shapes::{corrupt, PbRng};
use serde_json::Value;

fn unhex(s: &str) -> Vec<u8> {
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
}

/// Returns whether the bytes were accepted.
fn probe(ty: &str, bytes: &[u8]) -> bool {
    with_pb_type!(ty, T => {
        let valid = psio::validate_protobuf::<T>(bytes);
        let decoded = psio::from_protobuf::<T>(bytes);
        assert_eq!(decoded.as_ref().err(), valid.as_ref().err());
        match decoded {
            Err(_) => false,
            Ok(value) => {
                let once = psio::to_protobuf(&value);
                assert_eq!(psio::protobuf_size(&value), once.len());
                let again = psio::from_protobuf::<T>(&once).expect("what psio writes, it reads");
                assert_eq!(psio::to_protobuf(&again), once);
                true
            }
        }
    })
}

#[test]
fn truncations_replaced_bytes_corruptions_and_random_bytes() {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../conformance/protobuf-vectors.json");
    let fx: Value = serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap();
    let mut rng = PbRng::new(0xf5);
    let (mut inputs, mut accepted) = (0u64, 0u64);
    let mut count = |ok: bool| {
        inputs += 1;
        accepted += ok as u64;
    };
    for row in fx["accept"].as_array().unwrap() {
        let ty = row["type"].as_str().unwrap();
        let base = unhex(row["wire_hex"].as_str().unwrap());
        for n in 0..=base.len() {
            count(probe(ty, &base[..n]));
        }
        let mut c = base.clone();
        for at in 0..base.len() {
            for _ in 0..2 {
                c[at] = rng.below(256) as u8;
                count(probe(ty, &c));
            }
            c[at] = base[at] ^ 0x80;
            count(probe(ty, &c));
            c[at] = base[at];
        }
        for _ in 0..200 {
            count(probe(ty, &corrupt(&base, &mut rng)));
        }
    }
    for ty in ["PbScalars", "PbRepeated", "PbMaps", "PbChoice", "PbTree", "PbOrder", "PbEncodings", "PbText"] {
        for _ in 0..20_000 {
            let n = rng.below(48) as usize;
            let bytes: Vec<u8> =
                (0..n).map(|_| if rng.below(100) < 60 { rng.below(32) } else { rng.below(256) } as u8).collect();
            count(probe(ty, &bytes));
        }
    }
    println!("{inputs} inputs, {accepted} accepted");
    assert!(inputs > 300_000);
    assert!(accepted > 30_000);
}

#[test]
fn work_is_bounded_by_the_input() {
    use protobuf_shapes::{PbMaps, PbOrder, PbPoint, PbRepeated, PbTree};
    // A length that claims 2 GiB: refused at the length.
    for tag in [0x0au8, 0x12, 0x1a, 0x22, 0x2a, 0x32, 0x3a, 0x42, 0x4a] {
        let claim = [tag, 0xff, 0xff, 0xff, 0xff, 0x07, 1];
        assert!(psio::validate_protobuf::<PbRepeated>(&claim).is_err());
        assert!(psio::from_protobuf::<PbOrder>(&claim).is_err());
        assert!(psio::from_protobuf::<PbMaps>(&claim).is_err());
    }
    // Megabytes of unknown fields, groups, empty records and entries.
    let repeated = |piece: &[u8], count: usize| piece.repeat(count);
    assert!(psio::from_protobuf::<PbRepeated>(&repeated(&[0xf8, 0x07, 0x01], 1_000_000)).is_ok());
    assert!(psio::from_protobuf::<PbRepeated>(&repeated(&[0xfb, 0x07, 0xfc, 0x07], 1_000_000)).is_ok());
    let nested = psio::from_protobuf::<PbRepeated>(&repeated(&[0xfb, 0x07], 1_000_000)).unwrap_err();
    assert_eq!((nested.message(), nested.offset()), ("protobuf: nested too deeply", 202));
    assert_eq!(psio::from_protobuf::<PbRepeated>(&repeated(&[0x32, 0x00], 1_000_000)).unwrap().points.len(), 1_000_000);
    assert_eq!(psio::from_protobuf::<PbMaps>(&repeated(&[0x0a, 0x00], 1_000_000)).unwrap().counts.len(), 1);
    assert_eq!(psio::from_protobuf::<PbTree>(&repeated(&[0x12, 0x00], 1_000_000)).unwrap().children.len(), 1_000_000);
    // The depth limit is an option.
    let mut deep = Vec::new();
    for _ in 0..40 {
        let mut outer = vec![0x12, deep.len() as u8];
        outer.extend_from_slice(&deep);
        deep = outer;
    }
    use psio::protobuf::PbLimits;
    assert!(psio::validate_protobuf_with::<PbTree>(&deep, PbLimits { max_depth: 40 }).is_ok());
    assert!(psio::validate_protobuf_with::<PbTree>(&deep, PbLimits { max_depth: 39 }).is_err());
    assert_eq!(psio::from_protobuf::<PbPoint>(&[]).unwrap(), PbPoint { x: 0, y: 0 });
}
