//! The key / time features and the schema-fidelity features together
//! (the Rust side of cpp/tests/key_time_schema_tests.cpp):
//!
//! * a time id is a built-in presentation of the Custom-id mechanism: the
//!   tick integer on the wire, the text of docs/time.md, never a handler;
//! * a field-less enum is its underlying integer in a key; its names are
//!   JSON only;
//! * raw and UTF-8 strings are the same bytes in pssz and key, distinct ids
//!   in the schema, and only the UTF-8 one is validated;
//! * `pssz_evolves_from` tells an integer from a time value.

use psio::json::{self, IJson, Json};
use psio::key::{from_key, to_key, validate_key};
use psio::pssz::dynamic::{self, CustomHandlers};
use psio::pssz::schema_ir::{schema_for, AnyType, AnyValue, Custom, Int, List, Member, Schema};
use psio::pssz::{pssz_evolves_from, try_to_pssz, Pssz, RawString};
use psio::time::{Duration, Micros, Millis, Seconds, TimePoint};

#[derive(
    Debug, Clone, Copy, PartialEq, Eq, psio::JsonText, psio::PsszPack, psio::PsszUnpack,
    psio::ToPsszSchema, psio::Key,
)]
#[repr(i32)]
enum Status {
    Pending = -1,
    Active = 2,
    Closed = 7,
}

#[derive(
    Debug, Clone, Copy, PartialEq, Eq, psio::JsonText, psio::PsszPack, psio::PsszUnpack,
    psio::ToPsszSchema, psio::Key,
)]
#[repr(u8)]
enum Open {
    Red = 1,
    Blue = 9,
    #[pssz(other)]
    Unknown(u8),
}

#[derive(
    Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema,
    psio::Key,
)]
struct Order {
    status: Status,
    label: String,
    raw: RawString,
    at: TimePoint<Micros>,
    ttl: Duration<Millis>,
}

fn sample() -> Order {
    Order {
        status: Status::Active,
        label: "café".into(),
        raw: RawString(b"plain".to_vec()),
        at: TimePoint::from_ticks(1_795_342_530_123_456),
        ttl: Duration::from_ticks(-1500),
    }
}

#[test]
fn an_enum_is_its_underlying_integer_in_a_key() {
    for (status, value) in [(Status::Pending, -1i32), (Status::Active, 2), (Status::Closed, 7)] {
        assert_eq!(to_key(&status), to_key(&value));
        assert_eq!(from_key::<Status>(&to_key(&value)).unwrap(), status);
        assert_eq!(try_to_pssz::<Pssz, _>(&status).unwrap(), try_to_pssz::<Pssz, _>(&value).unwrap());
    }
    // Key order is the order of the values.
    assert!(to_key(&Status::Pending) < to_key(&Status::Active));
    assert!(to_key(&Status::Active) < to_key(&Status::Closed));
    // A value that is no enumerator is not a key of a closed enum ...
    assert!(from_key::<Status>(&to_key(&3i32)).is_err());
    assert!(validate_key::<Status>(&to_key(&3i32)).is_err());
    // ... and is the catch-all of an open one, in the integer's key form.
    assert_eq!(to_key(&Open::Blue), to_key(&9u8));
    assert_eq!(to_key(&Open::Unknown(200)), to_key(&200u8));
    assert_eq!(from_key::<Open>(&to_key(&200u8)).unwrap(), Open::Unknown(200));
    assert_eq!(from_key::<Open>(&to_key(&9u8)).unwrap(), Open::Blue);
    assert!(to_key(&Open::Red) < to_key(&Open::Unknown(5)));
    assert!(to_key(&Open::Unknown(5)) < to_key(&Open::Blue));
    // The names are text only.
    assert_eq!(json::to_string::<Json, _>(&Status::Active).unwrap(), "\"Active\"");
    // In a record the enum takes the integer's place.
    let order = sample();
    let as_tuple = (2i32, "café".to_string(), RawString(b"plain".to_vec()), order.at, order.ttl);
    assert_eq!(to_key(&order), to_key(&as_tuple));
    assert_eq!(from_key::<Order>(&to_key(&order)).unwrap(), order);
}

#[test]
fn raw_and_utf8_strings_share_bytes_and_differ_in_validation() {
    let text = "café".to_string();
    let raw = RawString(text.clone().into_bytes());
    assert_eq!(to_key(&text), to_key(&raw));
    assert_eq!(try_to_pssz::<Pssz, _>(&text).unwrap(), try_to_pssz::<Pssz, _>(&raw).unwrap());
    let junk = RawString(vec![b'a', 0, 0xff]);
    let key = to_key(&junk);
    assert_eq!(from_key::<RawString>(&key).unwrap(), junk);
    assert!(from_key::<String>(&key).is_err());
    assert!(validate_key::<String>(&key).is_err());

    let (schema, root) = schema_for::<Order>().unwrap();
    let AnyValue::Object(object) = &schema.resolve(&root).unwrap().value else { panic!("not an object") };
    let id_of = |name: &str| {
        let member = object.members.iter().find(|m| m.name == name).unwrap();
        match &schema.resolve(&member.r#type).unwrap().value {
            AnyValue::Custom(custom) => custom.id.clone(),
            AnyValue::Enum(_) => "<enum>".into(),
            other => panic!("{name}: {other:?}"),
        }
    };
    assert_eq!(id_of("label"), "utf8");
    assert_eq!(id_of("raw"), "string");
    assert_eq!(id_of("status"), "<enum>");
    assert_eq!(id_of("at"), "time-point-micros");
    assert_eq!(id_of("ttl"), "duration-millis");
}

#[test]
fn the_schema_driven_codec_presents_time_ids_without_handlers() {
    let (schema, root) = schema_for::<Order>().unwrap();
    let value = sample();
    let bytes = try_to_pssz::<Pssz, _>(&value).unwrap();
    let text = json::to_string::<Json, _>(&value).unwrap();
    assert_eq!(
        text,
        r#"{"status":"Active","label":"café","raw":"plain","at":"2026-11-22T10:15:30.123456Z","ttl":-1500}"#
    );
    assert_eq!(json::to_string::<IJson, _>(&value).unwrap(), text);
    assert_eq!(dynamic::validate_pssz(&schema, &root, &bytes), Ok(false));
    for strict in [false, true] {
        let mut handlers = CustomHandlers::new();
        handlers.set_strict(strict);
        // A handler under a time id is never consulted.
        handlers.add(
            "time-point-micros",
            |_: &[u8]| Ok("hijacked".to_string()),
            |_: &str| Ok(vec![0u8; 8]),
        );
        assert_eq!(dynamic::pssz_to_json_with::<Json>(&schema, &root, &bytes, &handlers).unwrap().0, text);
        assert_eq!(dynamic::pssz_to_json_with::<IJson>(&schema, &root, &bytes, &handlers).unwrap().0, text);
        assert_eq!(
            dynamic::json_to_pssz_with::<Json>(&schema, &root, text.as_bytes(), &handlers).unwrap(),
            bytes
        );
        assert_eq!(
            dynamic::json_to_pssz_with::<IJson>(&schema, &root, text.as_bytes(), &handlers).unwrap(),
            bytes
        );
    }
    assert_eq!(dynamic::pssz_to_json::<Json>(&schema, &root, &bytes).unwrap().0, text);

    // Every text rule of the typed codec holds through the schema.
    let with_at = |at: &str| {
        format!(r#"{{"status":2,"label":"","raw":"","at":{at},"ttl":0}}"#)
    };
    for good in [
        r#""2026-11-22T10:15:30.123456Z""#,
        r#""2026-11-22t10:15:30.1234560z""#,
        r#""2026-11-22T12:15:30.123456+02:00""#,
    ] {
        let typed = json::from_str::<Json, Order>(&with_at(good)).unwrap();
        assert_eq!(typed.at, value.at, "{good}");
        assert_eq!(
            dynamic::json_to_pssz::<Json>(&schema, &root, with_at(good).as_bytes()).unwrap(),
            try_to_pssz::<Pssz, _>(&typed).unwrap(),
            "{good}"
        );
    }
    for bad in [
        "1795342530123456",
        r#""2026-11-22T10:15:30.1234567Z""#,
        r#""2026-11-22T10:15:60Z""#,
        r#""2026-11-22 10:15:30Z""#,
        r#""2026-02-29T00:00:00Z""#,
        "null",
    ] {
        assert!(json::from_str::<Json, Order>(&with_at(bad)).is_err(), "{bad}");
        assert!(dynamic::json_to_pssz::<Json>(&schema, &root, with_at(bad).as_bytes()).is_err(), "{bad}");
        assert!(dynamic::json_to_pssz::<IJson>(&schema, &root, with_at(bad).as_bytes()).is_err(), "{bad}");
    }

    // A time point outside years 0000..9999 has bytes and a key, no text.
    let far = Order { at: TimePoint::from_ticks(i64::MAX), ..sample() };
    let far_bytes = try_to_pssz::<Pssz, _>(&far).unwrap();
    assert_eq!(dynamic::validate_pssz(&schema, &root, &far_bytes), Ok(false));
    assert!(json::to_string::<Json, _>(&far).is_err());
    assert!(dynamic::pssz_to_json::<Json>(&schema, &root, &far_bytes).is_err());
    assert_eq!(from_key::<Order>(&to_key(&far)).unwrap(), far);
}

fn custom_schema(id: &str, inner: AnyValue) -> (Schema, AnyType) {
    let mut schema = Schema::default();
    schema.types.push(Member {
        name: "T".into(),
        r#type: Box::new(AnyType::new(AnyValue::Custom(Custom {
            r#type: Box::new(AnyType::new(inner)),
            id: id.into(),
        }))),
        attributes: vec![],
    });
    (schema, AnyType::named("T"))
}

#[test]
fn time_ids_are_reserved() {
    let int = |bits, signed| AnyValue::Int(Int { bits, isSigned: signed });
    let accepts = |id: &str, inner: AnyValue| {
        let (schema, root) = custom_schema(id, inner);
        let binary = dynamic::validate_pssz(&schema, &root, &[0u8; 8]).is_ok();
        let write = dynamic::pssz_to_json::<Json>(&schema, &root, &[0u8; 8]).is_ok();
        let read = dynamic::json_to_pssz::<Json>(&schema, &root, b"0").is_ok();
        assert_eq!(binary, write, "{id}");
        binary || write || read
    };
    assert!(accepts("duration-millis", int(64, true)));
    assert!(accepts("duration-60/1", int(64, false)));
    assert!(accepts("time-point-seconds", int(64, true)));
    // An ordinary custom id is its inner type.
    assert!(accepts("AccountNumber", int(64, false)));
    for id in [
        "time-point", "duration", "time-point-", "duration-", "duration-minutes",
        "duration-1/1000", "duration-2/4", "time-point-01/7", "duration-0/1",
    ] {
        assert!(!accepts(id, int(64, true)), "{id}");
    }
    assert!(!accepts("duration-millis", int(128, true)));
    assert!(!accepts(
        "time-point-seconds",
        AnyValue::Custom(Custom {
            r#type: Box::new(AnyType::new(AnyValue::List(List {
                r#type: Box::new(AnyType::new(int(8, false))),
            }))),
            id: "utf8".into(),
        })
    ));
}

#[derive(Debug, PartialEq, psio::PsszPack, psio::PsszUnpack)]
struct AsInt {
    id: u32,
    at: i64,
}
#[derive(Debug, PartialEq, psio::PsszPack, psio::PsszUnpack)]
struct AsMicros {
    id: u32,
    at: TimePoint<Micros>,
}
#[derive(Debug, PartialEq, psio::PsszPack, psio::PsszUnpack)]
struct AsMicrosGrown {
    id: u32,
    at: TimePoint<Micros>,
    note: Option<u8>,
}
#[derive(Debug, PartialEq, psio::PsszPack, psio::PsszUnpack)]
struct AsMillis {
    id: u32,
    at: TimePoint<Millis>,
}
#[derive(Debug, PartialEq, psio::PsszPack, psio::PsszUnpack)]
struct AsDuration {
    id: u32,
    at: Duration<Micros>,
}

// C++: psio::pssz_evolves_from<int64_t, sys_time<microseconds>> is false —
// the two are different types that happen to share bytes.
const _: () = {
    assert!(pssz_evolves_from::<i64, i64>());
    assert!(pssz_evolves_from::<TimePoint<Micros>, TimePoint<Micros>>());
    assert!(pssz_evolves_from::<Duration<Seconds, u32>, Duration<Seconds, u32>>());
    assert!(!pssz_evolves_from::<i64, TimePoint<Micros>>());
    assert!(!pssz_evolves_from::<TimePoint<Micros>, i64>());
    assert!(!pssz_evolves_from::<i64, Duration<Micros>>());
    assert!(!pssz_evolves_from::<TimePoint<Micros>, TimePoint<Millis>>());
    assert!(!pssz_evolves_from::<TimePoint<Micros>, Duration<Micros>>());
    assert!(!pssz_evolves_from::<TimePoint<Seconds>, TimePoint<Seconds, u64>>());
    assert!(!pssz_evolves_from::<Duration<Seconds, u32>, Duration<Seconds, i32>>());
    assert!(!pssz_evolves_from::<AsInt, AsMicros>());
    assert!(!pssz_evolves_from::<AsMicros, AsInt>());
    assert!(!pssz_evolves_from::<AsMicros, AsMillis>());
    assert!(!pssz_evolves_from::<AsMicros, AsDuration>());
    assert!(pssz_evolves_from::<AsMicros, AsMicros>());
    assert!(pssz_evolves_from::<AsMicros, AsMicrosGrown>());
    assert!(!pssz_evolves_from::<AsInt, AsMicrosGrown>());
    assert!(pssz_evolves_from::<Option<TimePoint<Micros>>, Option<TimePoint<Micros>>>());
    assert!(!pssz_evolves_from::<Vec<i64>, Vec<TimePoint<Micros>>>());
};

#[test]
fn evolution_tells_an_integer_from_a_time_value() {
    // The const block above is the test; the bytes are indeed shared.
    let a = try_to_pssz::<Pssz, _>(&AsInt { id: 1, at: 5 }).unwrap();
    let b = try_to_pssz::<Pssz, _>(&AsMicros { id: 1, at: TimePoint::from_ticks(5) }).unwrap();
    assert_eq!(a, b);
}
