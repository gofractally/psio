//! The record member attributes of the numbered formats (protobuf, Cap'n
//! Proto, FlatBuffers): `field`, `default`, `deprecated`. The schema-driven
//! PSSZ codec accepts them on a record member and does not read them; the
//! schema derive writes them from `#[psio(field = N)]`,
//! `#[psio(default = ..)]` and `#[psio(deprecated)]`; and `protobuf`
//! (`#[psio(pb_sint)]` / `#[psio(pb_fixed)]`), which protobuf alone reads.

#![allow(dead_code)]

use psio::pssz::dynamic::{self as codec, Compare, Value};
use psio::pssz::schema_ir::*;
use psio::pssz::{Pssz, Pssz16, Pssz32, Pssz8};

fn attribute(name: &str, value: &str) -> Attribute {
    Attribute { name: name.into(), value: Some(value.into()) }
}
fn int(bits: u32, signed: bool) -> AnyType {
    AnyType::new(AnyValue::Int(Int { bits, isSigned: signed }))
}
fn string() -> AnyType {
    AnyType::new(AnyValue::Custom(Custom {
        r#type: Box::new(AnyType::new(AnyValue::List(List { r#type: Box::new(int(8, false)) }))),
        id: "string".into(),
    }))
}
fn member(name: &str, ty: AnyType, attributes: Vec<Attribute>) -> Member {
    Member { name: name.into(), r#type: Box::new(ty), attributes }
}
fn object(members: Vec<Member>, attributes: Vec<Attribute>) -> AnyType {
    AnyType::new(AnyValue::Object(Object { members, attributes }))
}
fn variant(members: Vec<Member>, attributes: Vec<Attribute>) -> AnyType {
    AnyType::new(AnyValue::Variant(Variant { members, attributes }))
}
fn numbered() -> Vec<Attribute> {
    vec![
        attribute("field", "7"),
        attribute("deprecated", ""),
        attribute("default", "-1"),
        attribute("protobuf", "sint"),
    ]
}
fn row(attributes: [Vec<Attribute>; 2]) -> AnyType {
    let [id, name] = attributes;
    object(vec![member("id", int(32, true), id), member("name", string(), name)], vec![])
}

#[test]
fn a_record_member_with_them_is_the_same_type() {
    let schema = Schema::default();
    let plain = row([vec![], vec![]]);
    let stated = row([
        numbered(),
        vec![attribute("lengthBound", "max=9"), attribute("field", "2"), attribute("deprecated", "v2")],
    ]);
    let bounded = row([vec![], vec![attribute("lengthBound", "max=9")]]);
    let value =
        Value::Record(vec![("id".into(), Value::Signed(-5)), ("name".into(), Value::String("abc".into()))]);

    macro_rules! same {
        ($format:ty, $other:expr) => {{
            let bytes = codec::encode::<$format>(&schema, &stated, &value).unwrap();
            assert_eq!(bytes, codec::encode::<$format>(&schema, $other, &value).unwrap());
            assert_eq!(codec::decode_exact::<$format>(&schema, &stated, &bytes).unwrap(), value);
            assert_eq!(codec::decode_compatible::<$format>(&schema, &stated, &bytes).unwrap().value, value);
        }};
    }
    same!(Pssz8, &plain);
    same!(Pssz16, &plain);
    same!(Pssz32, &plain);
    // The automatic width reads the bound, and nothing of the three.
    same!(Pssz, &bounded);
    assert_eq!(codec::auto_width(&schema, &stated).unwrap(), codec::auto_width(&schema, &bounded).unwrap());
    let bytes = codec::encode::<Pssz>(&schema, &stated, &value).unwrap();
    assert_eq!(codec::validate(&schema, &stated, &bytes), codec::validate(&schema, &bounded, &bytes));
    assert_eq!(codec::validate(&schema, &stated, &bytes), Ok(false));

    // The same text.
    let text = codec::value_to_json::<psio::json::Json>(&schema, &stated, &value).unwrap();
    assert_eq!(text, codec::value_to_json::<psio::json::Json>(&schema, &bounded, &value).unwrap());
    assert_eq!(codec::json_to_value::<psio::json::Json>(&schema, &stated, text.as_bytes()).unwrap(), value);

    // And the same type to a comparison, either way round.
    for what in [Compare::Wire, Compare::Contract] {
        for (a, b) in [(&stated, &bounded), (&bounded, &stated)] {
            assert!(codec::equivalent(&schema, a, &schema, b, what).ok);
            assert!(codec::evolves_from(&schema, a, &schema, b, what).ok);
        }
    }
}

#[test]
fn they_belong_to_record_members_only() {
    let schema = Schema::default();
    let unit = Value::Record(vec![]);
    for name in ["field", "default", "deprecated", "protobuf"] {
        let stated = vec![attribute(name, "1")];
        let on_record = object(vec![], stated.clone());
        let on_variant = variant(vec![member("a", object(vec![], vec![]), vec![])], stated.clone());
        let on_alternative = variant(vec![member("a", object(vec![], vec![]), stated.clone())], vec![]);
        let alternative = Value::Variant { selector: 0, value: Box::new(unit.clone()) };
        for (ty, value) in [(&on_record, &unit), (&on_variant, &alternative), (&on_alternative, &alternative)] {
            assert_eq!(
                codec::encode::<Pssz32>(&schema, ty, value).unwrap_err().0,
                codec::UNSUPPORTED_CONSTRAINT,
                "{name}"
            );
            assert_eq!(codec::validate(&schema, ty, &[0; 8]).unwrap_err().0, codec::UNSUPPORTED_CONSTRAINT);
        }
    }
    // An unknown name on a record member is still rejected.
    let unknown = row([vec![attribute("fieldNumber", "1")], vec![]]);
    assert_eq!(codec::validate(&schema, &unknown, &[0; 8]).unwrap_err().0, codec::UNSUPPORTED_CONSTRAINT);
}

#[derive(Clone, Copy, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema)]
#[repr(i32)]
enum Status {
    Pending = -1,
    Idle = 0,
    Active = 2,
    #[psio(other)]
    Unknown(i32),
}

#[derive(Clone, Copy, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema)]
#[repr(u8)]
enum Level {
    Low,
    High,
}

#[derive(psio::ToPsszSchema)]
struct Stated {
    #[psio(field = 3)]
    id: u32,
    #[psio(deprecated)]
    old: u8,
    #[psio(field = 1, deprecated, default = 7)]
    #[json(json_string)]
    count: u64,
    #[psio(default = true)]
    on: bool,
    #[psio(default = -40)]
    low: i16,
    #[psio(default = "Status::Pending")]
    status: Status,
    #[psio(default = "Status::Unknown(9)")]
    other: Status,
    #[psio(default = "Level::High")]
    level: Level,
    #[psio(default = 0.5)]
    half: f32,
    #[psio(default = "f64::NEG_INFINITY")]
    bottom: f64,
    // A zero default is the absence of one.
    #[psio(default = 0)]
    zero: u32,
    #[psio(default = false)]
    off: bool,
    #[psio(default = "Status::Idle")]
    idle: Status,
    #[psio(default = 0.0)]
    nothing: f64,
    // How protobuf writes the member's integers: before `default`, as C++.
    #[psio(pb_sint, default = -2)]
    zig: i32,
    #[psio(field = 9, pb_fixed)]
    wide: Vec<u64>,
    plain: String,
}

fn members_of<T: ToPsszSchema>() -> Vec<Member> {
    let (schema, root) = schema_for::<T>().unwrap();
    match &schema.resolve(&root).unwrap().value {
        AnyValue::Object(o) => o.members.clone(),
        AnyValue::Struct(s) => s.members.clone(),
        _ => panic!("a record"),
    }
}

#[test]
fn the_derive_writes_them() {
    let written: Vec<(String, Vec<(String, String)>)> = members_of::<Stated>()
        .into_iter()
        .map(|m| {
            (m.name, m.attributes.into_iter().map(|a| (a.name, a.value.expect("a value"))).collect())
        })
        .collect();
    let expected: Vec<(&str, Vec<(&str, &str)>)> = vec![
        ("id", vec![("field", "3")]),
        ("old", vec![("deprecated", "")]),
        ("count", vec![("jsonInt", "string"), ("field", "1"), ("deprecated", ""), ("default", "7")]),
        ("on", vec![("default", "true")]),
        ("low", vec![("default", "-40")]),
        ("status", vec![("default", "-1")]),
        ("other", vec![("default", "9")]),
        ("level", vec![("default", "1")]),
        ("half", vec![("default", "0.5")]),
        ("bottom", vec![("default", "-inf")]),
        ("zero", vec![]),
        ("off", vec![]),
        ("idle", vec![]),
        ("nothing", vec![]),
        ("zig", vec![("protobuf", "sint"), ("default", "-2")]),
        ("wide", vec![("field", "9"), ("protobuf", "fixed")]),
        ("plain", vec![]),
    ];
    let expected: Vec<(String, Vec<(String, String)>)> = expected
        .into_iter()
        .map(|(n, a)| (n.to_string(), a.into_iter().map(|(k, v)| (k.to_string(), v.to_string())).collect()))
        .collect();
    assert_eq!(written, expected);

    // The schema-driven codec takes the derived schema as it is.
    let (schema, root) = schema_for::<Stated>().unwrap();
    codec::validate_schema(&schema).unwrap();
    assert!(codec::auto_width(&schema, &root).is_ok());
}

#[test]
fn the_text_of_a_float_default_is_the_shortest() {
    let text = |v: f64| v.default_text().unwrap();
    for (value, expected) in [
        (0.0, ""),
        (-0.0, "-0"),
        (f64::NAN, "nan"),
        (f64::INFINITY, "inf"),
        (f64::NEG_INFINITY, "-inf"),
        (1.0, "1"),
        (-2.5, "-2.5"),
        (0.1, "0.1"),
        (1234.5, "1234.5"),
        (10000.0, "10000"),
        (100000.0, "1e+05"),
        (123456.0, "123456"),
        (1e22, "1e+22"),
        (1.5e300, "1.5e+300"),
        (0.001, "0.001"),
        (0.0001, "1e-04"),
        (0.00015, "0.00015"),
        (1.25e-7, "1.25e-07"),
        (-3e-310, "-3e-310"),
        (12345678901234567890.0, "12345678901234567168"),
    ] {
        assert_eq!(text(value), expected, "{value:e}");
    }
    assert_eq!(0.1f32.default_text().unwrap(), "0.1");
    assert_eq!(16777216f32.default_text().unwrap(), "16777216");
    assert_eq!(3.4028235e38f32.default_text().unwrap(), "3.4028235e+38");
    assert_eq!(u64::MAX.default_text().unwrap(), "18446744073709551615");
    assert_eq!(String::new().default_text(), None);
}

#[derive(psio::ToPsszSchema)]
struct Misplaced {
    #[psio(default = "String::new()")]
    name: String,
}

#[test]
fn a_default_belongs_to_a_scalar_member() {
    assert_eq!(
        schema_for::<Misplaced>().unwrap_err().0,
        "pssz schema: #[psio(default = ..)] applies to a scalar member"
    );
}
