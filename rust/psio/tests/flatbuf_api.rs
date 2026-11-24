//! The FlatBuffers API of the crate: the mapping the derives implement, the
//! helpers, and the views. (The bytes are pinned by `flatbuf_conformance`,
//! behaviour on invalid bytes by `flatbuf_fuzz`.)

mod flatbuf_shapes;

use flatbuf_shapes::natural::*;
use psio::flatbuf::{FbLimits, FbOptions, FbPack, FbTable, FbView, FlatbufBuffer, SharedFlatbuf};
use psio::pssz::RawString;
use std::collections::BTreeMap;

#[test]
fn helpers() {
    let profile = sample_profile();
    let bytes = psio::to_flatbuf(&profile);
    assert_eq!(psio::flatbuf_size(&profile), bytes.len());
    assert!(psio::validate_flatbuf::<Profile>(&bytes).is_ok());
    assert_eq!(psio::from_flatbuf::<Profile>(&bytes).unwrap(), profile);
    assert_eq!(psio::from_flatbuf_trusted::<Profile>(&bytes), profile);
    let mut sink = vec![1, 2, 3];
    psio::to_flatbuf_into(&profile, &mut sink);
    assert_eq!(sink, bytes);

    // Untrusted bytes are verified before they are decoded.
    let e = psio::from_flatbuf::<Profile>(&bytes[..24]).unwrap_err();
    assert_eq!((e.message(), e.offset()), ("flatbuf: bad table size", 6));
    assert_eq!(e.to_string(), "flatbuf: bad table size (at byte 6)");
    // The trusted decode of bytes that are not valid is defined: no panic.
    let _ = psio::from_flatbuf_trusted::<Profile>(&bytes[..24]);
    let _ = psio::from_flatbuf_trusted::<Order>(&[0xff; 64]);

    let owned = FlatbufBuffer::<Profile>::new(bytes.clone()).unwrap();
    assert_eq!(owned.as_view().name(), "Alice Example");
    assert_eq!(owned.decode().unwrap(), profile);
    assert!(SharedFlatbuf::<Profile>::new(bytes[..24].to_vec()).is_err());
    assert_eq!(psio::decode::<psio::flatbuf::Flatbuf, Profile>(&psio::encode::<psio::flatbuf::Flatbuf, _>(&profile).unwrap()).unwrap(), profile);
}

#[test]
fn file_identifier_and_size_prefix() {
    let profile = sample_profile();
    let plain = psio::to_flatbuf(&profile);
    let opts = FbOptions { file_identifier: Some(*b"PROF"), size_prefixed: true };
    let framed = psio::to_flatbuf_with(&profile, &opts);
    let limits = FbLimits::default();
    assert!(psio::validate_flatbuf_with::<Profile>(&framed, &opts, limits).is_ok());

    let inner = psio::flatbuf_size_prefixed(&framed).unwrap();
    assert_eq!(inner.len() + 4, framed.len());
    assert_eq!(psio::flatbuf_identifier(inner), Some(*b"PROF"));
    assert!(psio::flatbuf_has_identifier(inner, b"PROF") && !psio::flatbuf_has_identifier(inner, b"ORDR"));
    assert_eq!(psio::from_flatbuf_trusted::<Profile>(inner), profile);
    assert_eq!(FbView::<Profile>::new_with(&framed, &opts, limits).unwrap().unpack(), profile);

    // A stream: the next buffer starts after the prefixed one.
    let mut stream = framed.clone();
    stream.extend_from_slice(&framed);
    assert_eq!(psio::flatbuf_size_prefixed(&stream).unwrap(), inner);
    assert!(psio::flatbuf_size_prefixed(&framed[..framed.len() - 1]).is_none());
    assert!(psio::flatbuf_size_prefixed(&[1, 0]).is_none());

    let wrong = FbOptions { file_identifier: Some(*b"ORDR"), size_prefixed: true };
    let e = psio::validate_flatbuf_with::<Profile>(&framed, &wrong, limits).unwrap_err();
    assert_eq!((e.message(), e.offset()), ("flatbuf: wrong file identifier", 8));
    let e = psio::validate_flatbuf_with::<Profile>(&plain, &opts, limits).unwrap_err();
    assert_eq!(e.message(), "flatbuf: size prefix does not match the buffer");
    assert!(psio::flatbuf_identifier(&[0; 7]).is_none());
}

#[test]
fn a_record_is_a_table_or_a_struct() {
    // definition_will_not_change and every member fixed: a struct.
    assert!(Vec3::STRUCT && !Vec3::TABLE);
    assert_eq!((Vec3::FIXED_SIZE, Vec3::FIXED_ALIGN), (12, 4));
    // flatc: struct Mixed { a:ubyte; b:ulong; c:ushort; d:[int:3]; v:Vec3; e:bool; }
    assert!(Mixed::STRUCT);
    assert_eq!((Mixed::FIXED_SIZE, Mixed::FIXED_ALIGN), (48, 8));
    assert!(Point::TABLE && Order::TABLE);
    assert_eq!(<Payment as FbPack>::SLOTS, 2);

    // definition_will_not_change with a member that is not fixed: a table,
    // as in C++.
    #[derive(Debug, PartialEq, psio::FbPack, psio::FbUnpack)]
    #[psio(definition_will_not_change)]
    struct Named {
        id: u32,
        name: String,
    }
    assert!(Named::TABLE);
    let value = Named { id: 3, name: "three".into() };
    assert_eq!(psio::from_flatbuf::<Named>(&psio::to_flatbuf(&value)).unwrap(), value);

    // A struct is stored inline: in its table, and in a vector.
    #[derive(Debug, PartialEq, psio::FbPack, psio::FbUnpack, psio::FbView)]
    struct Shape {
        at: Vec3,
        corners: Vec<Vec3>,
    }
    let shape = Shape { at: Vec3 { x: 1.0, y: 2.0, z: 3.0 }, corners: vec![Vec3 { x: 4.0, y: 5.0, z: 6.0 }; 2] };
    let bytes = psio::to_flatbuf(&shape);
    assert_eq!(psio::from_flatbuf::<Shape>(&bytes).unwrap(), shape);
    let view = FbView::<Shape>::new(&bytes).unwrap();
    assert_eq!((view.at().x(), view.at().z()), (1.0, 3.0));
    assert_eq!(view.corners().get(1).unwrap().unpack(), shape.corners[1]);
    assert_eq!(view.corners().unpack(), shape.corners);
}

#[test]
fn defaults_field_numbers_and_deprecated_members() {
    // A scalar equal to its default is not written; an absent one reads as it.
    let defaults: Defaults = empty();
    assert_eq!(defaults, Defaults { level: 7, on: true, c: Color::Green, ratio: 0.5 });
    let bytes = psio::to_flatbuf(&defaults);
    assert_eq!(bytes.len(), 12);
    assert_eq!(psio::from_flatbuf::<Defaults>(&bytes).unwrap(), defaults);
    let view = FbView::<Defaults>::new(&bytes).unwrap();
    assert_eq!((view.level(), view.on(), view.c(), view.ratio()), (7, true, Color::Green, 0.5));
    let zeros = Defaults { level: 0, on: false, c: Color::Red, ratio: 0.0 };
    let bytes = psio::to_flatbuf(&zeros);
    assert_eq!(psio::from_flatbuf::<Defaults>(&bytes).unwrap(), zeros);
    assert_eq!(FbView::<Defaults>::new(&bytes).unwrap().level(), 0);

    // Floats are compared by bits: -0.0 is written where the default is 0.0.
    let bytes = psio::to_flatbuf(&Longs { values: vec![], reals: vec![] });
    assert_eq!(bytes.len(), 12);
    #[derive(Debug, PartialEq, psio::FbPack, psio::FbUnpack)]
    struct Real {
        x: f64,
    }
    let negative_zero = psio::from_flatbuf::<Real>(&psio::to_flatbuf(&Real { x: -0.0 })).unwrap();
    assert!(negative_zero.x.is_sign_negative());
    assert_eq!(psio::to_flatbuf(&Real { x: 0.0 }).len(), 12);

    // #[psio(field = N)]: vtable slots in ascending order of the numbers.
    let value = Renumbered { c: 5, a: "five".into(), b: -5 };
    let bytes = psio::to_flatbuf(&value);
    let table = FbTable::root(&bytes);
    assert_eq!((table.read_str(0), table.read_i64(1, 0), table.read_i32(2, 0)), (Some("five"), -5, 5));
    let view = FbView::<Renumbered>::new(&bytes).unwrap();
    assert_eq!((view.c(), view.a(), view.b()), (5, "five", -5));

    // #[psio(deprecated)]: the slot is kept, the member neither written nor read.
    let bytes = psio::to_flatbuf(&WithDeprecated { a: 1, old: 99, c: 3 });
    assert_eq!(bytes, psio::to_flatbuf(&WithDeprecated { a: 1, old: 0, c: 3 }));
    let table = FbTable::root(&bytes);
    assert!(table.has_field(0) && !table.has_field(1) && table.has_field(2));
    assert_eq!(psio::from_flatbuf::<WithDeprecated>(&bytes).unwrap(), WithDeprecated { a: 1, old: 0, c: 3 });
    assert_eq!(FbView::<WithDeprecated>::new(&bytes).unwrap().old(), 0);
}

#[test]
fn enums() {
    // Without a catch-all variant only the enumerators are values.
    let mut bytes = psio::to_flatbuf(&Scalars { c: Color::Blue, ..empty() });
    let at = bytes.iter().rposition(|b| *b == 2).unwrap();
    bytes[at] = 9;
    let e = psio::validate_flatbuf::<Scalars>(&bytes).unwrap_err();
    assert_eq!((e.message(), e.offset() as usize), ("flatbuf: unknown enum value", at));
    // Unverified, it reads as the default.
    assert_eq!(FbView::<Scalars>::trusted(&bytes).c(), Color::Red);
    // With one, every value is — what a C++ enum holds.
    use flatbuf_shapes::raw;
    assert!(psio::validate_flatbuf::<raw::Scalars>(&bytes).is_ok());
    let value = psio::from_flatbuf::<raw::Scalars>(&bytes).unwrap();
    assert_eq!(value.c, raw::Color::Other(9));
    assert_eq!(psio::to_flatbuf(&value), bytes);
    // A declared value always decodes to its enumerator.
    let declared = psio::to_flatbuf(&raw::Scalars { c: raw::Color::Other(2), ..raw::empty() });
    assert_eq!(psio::from_flatbuf::<raw::Scalars>(&declared).unwrap().c, raw::Color::Blue);

    // An enum with no enumerator 0: a member of that type declares its default.
    #[derive(Debug, Clone, Copy, PartialEq, psio::FbPack, psio::FbUnpack, psio::FbView)]
    #[repr(i16)]
    enum Level {
        Low = -5,
        High = 300,
    }
    #[derive(Debug, PartialEq, psio::FbPack, psio::FbUnpack, psio::FbView)]
    struct Gauge {
        #[psio(default = "Level::High")]
        level: Level,
        history: Vec<Level>,
    }
    let gauge = Gauge { level: Level::Low, history: vec![Level::High, Level::Low] };
    let bytes = psio::to_flatbuf(&gauge);
    assert_eq!(psio::from_flatbuf::<Gauge>(&bytes).unwrap(), gauge);
    let view = FbView::<Gauge>::new(&bytes).unwrap();
    assert_eq!((view.level(), view.history().get(0)), (Level::Low, Some(Level::High)));
    let default = psio::to_flatbuf(&Gauge { level: Level::High, history: vec![] });
    assert_eq!(default.len(), 12);
    assert_eq!(FbView::<Gauge>::new(&default).unwrap().level(), Level::High);
}

#[test]
fn views_read_every_shape_in_place() {
    let order = sample_order();
    let bytes = psio::to_flatbuf(&order);
    let v = FbView::<Order>::new(&bytes).unwrap();
    assert_eq!(v.unpack(), order);

    // Scalars, strings, nested tables, structs.
    assert_eq!((v.id(), v.total(), v.color()), (1001, 54.47, Color::Blue));
    assert_eq!((v.customer().name(), v.customer().age()), ("Alice Example", 37));
    assert_eq!((v.position().y(), v.mixed().b(), v.mixed().v().z()), (2.0, 0x0102030405060708, 2.5));
    assert_eq!(v.mixed().d().iter().collect::<Vec<_>>(), [-1, 2, -3]);

    // Vectors.
    let items = v.items();
    assert_eq!((items.len(), items.is_empty()), (3, false));
    assert_eq!(items.get(1).map(|item| (item.product(), item.qty(), item.unit_price())), Some(("gadget", 1, 24.5)));
    assert!(items.get(3).is_none());
    assert_eq!(items.iter().map(|item| item.qty()).collect::<Vec<_>>(), [3, 1, 2]);
    assert_eq!(v.tags().iter().rev().collect::<Vec<_>>(), ["longer tag", "", "a"]);
    assert_eq!(v.blob().as_bytes(), [0, 1, 2, 253, 254, 255, 7]);
    assert_eq!(v.flags().unpack(), [true, false, true]);
    assert_eq!(v.quad().iter().collect::<Vec<_>>(), [1, 2, 3, 65535]);
    assert_eq!(v.colors().unpack(), [Color::Red, Color::Blue]);
    assert_eq!(v.path().get(1).unwrap().y(), 1.5);

    // Options: an optional scalar, an optional string, an optional struct.
    assert_eq!((v.discount(), v.note()), (Some(-15), Some("leave at the door")));
    assert_eq!(v.origin().map(|origin| origin.x()), Some(-1.0));

    // Unions.
    let payment = v.payment();
    assert_eq!((payment.tag(), payment.is_none()), (1, false));
    assert_eq!(payment.card().map(|card| (card.number(), card.cvv())), Some(("4111", 123)));
    assert!(payment.wire().is_none());
    assert_eq!(payment.unpack(), order.payment);
    assert_eq!(v.refund().card().unwrap().number(), "refund");

    // Sorted vectors: key lookup by binary search.
    let labels = v.labels();
    assert!(labels.contains("gift") && labels.contains("rush") && !labels.contains("gif"));
    assert_eq!((labels.find("rush"), labels.lower_bound("h"), labels.lower_bound("zzz")), (Some(1), 1, 2));
    assert_eq!(labels.iter().collect::<Vec<_>>(), ["gift", "rush"]);
    let ids = v.ids();
    assert_eq!((ids.find(&40), ids.find(&41), ids.get(2)), (Some(1), None, Some(500)));
    let stock = v.stock();
    assert_eq!((stock.get("washer"), stock.get("nut"), stock.get("screw")), (Some(250), Some(0), None));
    assert!(stock.contains_key("bolt") && stock.len() == 3);
    assert_eq!(stock.iter().collect::<Vec<_>>(), [("bolt", 17), ("nut", 0), ("washer", 250)]);
    let by_id = v.by_id();
    assert_eq!(by_id.get(&7).map(|item| item.product()), Some("seven"));
    assert_eq!(by_id.get(&0).map(|item| item.qty()), Some(1));
    assert_eq!(by_id.get(&4_000_000_000).unwrap().unpack(), order.by_id[&4_000_000_000]);
    assert_eq!((by_id.key(1), by_id.find(&8), by_id.lower_bound(&8)), (Some(7), None, 2));

    // What is absent reads as its default; there is no null to test.
    let bytes = psio::to_flatbuf(&empty::<Order>());
    let v = FbView::<Order>::new(&bytes).unwrap();
    assert_eq!((v.id(), v.color(), v.discount(), v.note()), (0, Color::Green, None, None));
    assert_eq!((v.customer().name(), v.customer().id(), v.items().len()), ("", 0, 0));
    assert!(v.payment().is_none() && v.origin().is_none() && v.labels().is_empty() && v.stock().is_empty());
    assert_eq!(v.payment().unpack(), Payment::None);
    assert_eq!(v.unpack(), empty::<Order>());

    // A view of bytes nobody verified reads inside them, whatever they are.
    for garbage in [&[][..], &[0xff; 3], &[4, 0, 0, 0, 0xff, 0xff, 0xff, 0x7f], &[0x10; 64]] {
        let v = FbView::<Order>::trusted(garbage);
        let _ = (v.id(), v.customer().name(), v.items().get(0).map(|i| i.qty()), v.stock().get("x"), v.unpack());
    }
}

#[test]
fn unions_and_optionals_on_the_wire() {
    let mut order: Order = empty();
    // NONE: neither the type nor the value is written.
    let none = psio::to_flatbuf(&order);
    order.payment = Payment::Wire(Wire { iban: "DE".into() });
    order.discount = Some(0);
    order.note = Some(String::new());
    let bytes = psio::to_flatbuf(&order);
    assert!(bytes.len() > none.len());
    // An engaged option is always written, even zero or empty.
    let back = psio::from_flatbuf::<Order>(&bytes).unwrap();
    assert_eq!((back.discount, back.note.as_deref()), (Some(0), Some("")));
    assert_eq!(back, order);
    let view = FbView::<Order>::new(&bytes).unwrap();
    assert_eq!((view.payment().tag(), view.discount(), view.note()), (2, Some(0), Some("")));
    assert_eq!(view.payment().wire().unwrap().iban(), "DE");
    // A union without NONE reads as its first alternative when it is absent.
    assert_eq!(psio::from_flatbuf::<Order>(&none).unwrap().refund, Refund::Card(empty()));
}

#[test]
fn maps_generics_tuples_and_raw_strings() {
    // The string key of a map entry is required: it is written even when
    // empty. A scalar key equal to zero is not written.
    #[derive(Debug, PartialEq, psio::FbPack, psio::FbUnpack, psio::FbView)]
    struct Index {
        by_name: BTreeMap<String, Option<u16>>,
        by_code: BTreeMap<i8, Vec<String>>,
        raw: BTreeMap<RawString, bool>,
    }
    let index = Index {
        by_name: [("".to_string(), None), ("a".to_string(), Some(0))].into(),
        by_code: [(-1, vec!["minus".to_string()]), (0, vec![]), (5, vec!["".to_string()])].into(),
        raw: [(RawString(vec![0xff, 0x00]), true), (RawString(vec![]), false)].into(),
    };
    let bytes = psio::to_flatbuf(&index);
    assert_eq!(psio::from_flatbuf::<Index>(&bytes).unwrap(), index);
    let view = FbView::<Index>::new(&bytes).unwrap();
    assert_eq!((view.by_name().get(""), view.by_name().get("a")), (Some(None), Some(Some(0))));
    assert_eq!(view.by_code().get(&-1).unwrap().get(0), Some("minus"));
    assert_eq!(view.by_code().get(&0).map(|list| list.len()), Some(0));
    assert_eq!(view.raw().get(&[0xff, 0x00][..]), Some(true));
    assert_eq!(view.raw().key(0), Some(&[][..]));

    // A record with a type parameter.
    #[derive(Debug, PartialEq, psio::FbPack, psio::FbUnpack)]
    struct Pair<T> {
        first: T,
        rest: Vec<T>,
    }
    let pair = Pair { first: 7u16, rest: vec![1, 2] };
    assert_eq!(psio::from_flatbuf::<Pair<u16>>(&psio::to_flatbuf(&pair)).unwrap(), pair);
    let pair = Pair { first: "a".to_string(), rest: vec![String::new()] };
    assert_eq!(psio::from_flatbuf::<Pair<String>>(&psio::to_flatbuf(&pair)).unwrap(), pair);

    // A tuple struct: members by position.
    #[derive(Debug, PartialEq, psio::FbPack, psio::FbUnpack)]
    struct Tagged(u32, String);
    let tagged = Tagged(4, "four".into());
    let bytes = psio::to_flatbuf(&tagged);
    assert_eq!(psio::from_flatbuf::<Tagged>(&bytes).unwrap(), tagged);
    assert_eq!(FbTable::root(&bytes).read_str(1), Some("four"));

    // String is UTF-8, RawString any bytes.
    let raw = flatbuf_shapes::raw::Wire { iban: RawString(vec![0xc3, 0x28]) };
    let bytes = psio::to_flatbuf(&raw);
    assert_eq!(psio::from_flatbuf::<flatbuf_shapes::raw::Wire>(&bytes).unwrap(), raw);
    assert_eq!(psio::validate_flatbuf::<Wire>(&bytes).unwrap_err().message(), "flatbuf: string is not valid UTF-8");
    assert_eq!(FbView::<Wire>::trusted(&bytes).iban(), "");
}
