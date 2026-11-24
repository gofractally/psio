//! The protobuf API of the crate: the helper names, the mapping of the
//! derive, and the rules that are decisions (docs/protobuf.md).

mod protobuf_shapes;

use protobuf_shapes::*;
use psio::Protobuf;
use std::collections::BTreeMap;

#[test]
fn zero_values_are_absent_and_absent_is_zero() {
    let zero = psio::from_protobuf::<PbOrder>(&[]).unwrap();
    assert!(psio::to_protobuf(&zero).is_empty());
    assert_eq!(zero.scalars.color, PbColor::Red);
    assert_eq!(zero.scalars.level, PbLevel::Other(0)); // PbLevel names no zero
    assert_eq!(zero.choices, vec![]);
    // -0.0 is not the zero value.
    let mut n = psio::from_protobuf::<PbScalars>(&[]).unwrap();
    n.d = -0.0;
    assert_eq!(psio::to_protobuf(&n).len(), 9);
}

#[test]
fn fields_are_written_in_number_order() {
    let v = PbRenumbered { a: 1, b: "b".into(), c: 2, d: true, old: 9 };
    let bytes = psio::to_protobuf(&v);
    assert_eq!(bytes, [0x10, 9, 0x1a, 1, b'b', 0x38, 1, 0x80, 0x01, 1, 0xc0, 0x3e, 2]);
    let shuffled = [0xc0, 0x3e, 2, 0x38, 1, 0x80, 0x01, 1, 0x1a, 1, b'b', 0x10, 9];
    assert_eq!(psio::from_protobuf::<PbRenumbered>(&shuffled).unwrap(), v);
}

#[test]
fn option_is_explicit_presence() {
    let v = psio::from_protobuf::<PbOptionals>(&[0x08, 0, 0x1a, 0]).unwrap();
    assert_eq!((v.i, v.s.as_deref(), v.b), (Some(0), Some(""), None));
    assert_eq!(psio::to_protobuf(&v), [0x08, 0, 0x1a, 0]);
}

#[test]
fn an_enum_with_data_is_a_oneof() {
    let mut v = psio::from_protobuf::<PbChoice>(&[]).unwrap();
    // Absent: the unit variant, or the first variant's zero.
    assert_eq!((&v.pick, &v.always), (&PbPick::None, &PbAlways::Bool(false)));
    assert_eq!(psio::to_protobuf(&v), [0x50, 0]);
    v.pick = PbPick::String("x".into());
    v.always = PbAlways::Double(2.5);
    let bytes = psio::to_protobuf(&v);
    assert_eq!(bytes, [0x32, 1, b'x', 0x59, 0, 0, 0, 0, 0, 0, 4, 0x40]);
    assert_eq!(psio::from_protobuf::<PbChoice>(&bytes).unwrap(), v);
    // The last alternative wins; the same record alternative twice merges.
    let last = psio::from_protobuf::<PbChoice>(&[0x32, 1, b'x', 0x28, 7]).unwrap();
    assert_eq!(last.pick, PbPick::Int32(7));
    let merged = psio::from_protobuf::<PbChoice>(&[0x3a, 2, 0x08, 1, 0x3a, 2, 0x10, 2]).unwrap();
    assert_eq!(merged.pick, PbPick::Point(PbPoint { x: 1, y: 2 }));
}

#[test]
fn a_map_is_entries_in_key_order() {
    let mut v = psio::from_protobuf::<PbMaps>(&[]).unwrap();
    v.counts = BTreeMap::from([("b".to_string(), 2), ("a".to_string(), 0)]);
    let bytes = psio::to_protobuf(&v);
    assert_eq!(bytes, [0x0a, 5, 0x0a, 1, b'a', 0x10, 0, 0x0a, 5, 0x0a, 1, b'b', 0x10, 2]);
    assert_eq!(psio::from_protobuf::<PbMaps>(&bytes).unwrap(), v);
}

// An enum without a catch-all holds its enumerators only.
#[derive(Debug, Clone, Copy, PartialEq, Protobuf)]
#[repr(u8)]
enum Closed {
    A,
    B,
}
#[derive(Debug, PartialEq, Protobuf)]
struct WithClosed {
    e: Closed,
    #[psio(pb_fixed)]
    ids: std::collections::BTreeSet<u32>,
}

#[test]
fn an_enum_without_a_catch_all_refuses_other_values() {
    let v = psio::from_protobuf::<WithClosed>(&[0x08, 1, 0x12, 8, 2, 0, 0, 0, 1, 0, 0, 0]).unwrap();
    assert_eq!(v.e, Closed::B);
    assert_eq!(v.ids.len(), 2);
    assert_eq!(psio::to_protobuf(&v), [0x08, 1, 0x12, 8, 1, 0, 0, 0, 2, 0, 0, 0]);
    let e = psio::validate_protobuf::<WithClosed>(&[0x08, 2]).unwrap_err();
    assert_eq!((e.message(), e.offset()), ("protobuf: a value does not fit its type", 2));
    assert_eq!(e.to_string(), "protobuf: a value does not fit its type (at byte 2)");
    // With the catch-all (PbColor), the value is kept.
    let open = psio::from_protobuf::<PbScalars>(&[0x68, 0x4d]).unwrap();
    assert_eq!(open.color, PbColor::Other(77));
}

#[test]
fn the_format_tag() {
    use psio::protobuf::ProtobufFormat;
    let v = PbPoint { x: 1, y: -1 };
    let bytes = psio::encode::<ProtobufFormat, _>(&v).unwrap();
    assert_eq!(bytes, psio::to_protobuf(&v));
    assert_eq!(psio::decode::<ProtobufFormat, PbPoint>(&bytes).unwrap(), v);
    assert!(psio::protobuf::pb_numbers_valid(536870911, 1));
    assert!(!psio::protobuf::pb_numbers_valid(536870911, 2));
    assert!(!psio::protobuf::pb_numbers_valid(18999, 2));
    assert!(!psio::protobuf::pb_numbers_valid(0, 1));
}
