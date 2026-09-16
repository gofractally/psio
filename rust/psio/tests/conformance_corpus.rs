//! Public-library interoperability with cpp/conformance/cpp_golden.cpp.
//! The incompatible draft-spec harness is retained in conformance/history.
use psio::{Pack, Unpack, FracViewType};
use psio::ssz::{SszPack, SszUnpack};
use psio::capnp::{CapnpPack, CapnpUnpack};
use psio::flatbuf::{FbPack, FbUnpack};
use psio::wit::pack::WitPack;

#[derive(Debug, PartialEq, Default, psio::Pack, psio::Unpack,
         psio::CapnpPack, psio::CapnpUnpack, psio::FbPack, psio::FbUnpack,
         psio::WitPack)]
#[fracpack(fracpack_mod = "psio")]
struct Record { id: u32, name: String, values: Vec<u32>, active: bool }
psio::ssz_struct!(Record { id: u32, name: String, values: Vec<u32>, active: bool });
psio::pssz_struct!(Record { id: u32, name: String, values: Vec<u32>, active: bool });

#[derive(Debug, PartialEq, psio::Pack, psio::Unpack)]
#[fracpack(fracpack_mod = "psio")]
enum Choice { Number(u32), Text(String) }
#[derive(Debug, PartialEq, psio::Pack, psio::Unpack)]
#[fracpack(fracpack_mod = "psio")]
struct Envelope { head: Record, rows: Vec<Record>, note: Option<String>, choice: Choice }

psio::pjson_struct!(Record { id: u32, name: String, values: Vec<u32>, active: bool });
#[derive(Debug, PartialEq)]
struct Rows { rows: Vec<Record> }
psio::pjson_struct!(Rows { rows: Vec<Record> });
#[derive(Debug, PartialEq)]
struct Strings { values: Vec<String> }
psio::pjson_struct!(Strings { values: Vec<String> });
#[derive(Debug, PartialEq)]
struct Bytes { values: Vec<u8> }
psio::pjson_struct!(Bytes { values: Vec<u8> });

fn fixtures(format: &str) -> Vec<(String, Vec<u8>)> {
    let all: serde_json::Value = serde_json::from_str(include_str!("fixtures/cpp-golden.json")).unwrap();
    all.as_array().unwrap().iter().filter(|f| f["format"] == format)
        .map(|f| (f["id"].as_str().unwrap().to_owned(),
                  hex::decode(f["wire_hex"].as_str().unwrap()).unwrap())).collect()
}
fn record(id: &str) -> Record {
    match id {
        "record" => Record { id: 42, name: "Alice".into(), values: vec![1,256,65536], active: true },
        "empty_record" => Record::default(),
        _ => panic!("Unknown C++ record fixture {id}"),
    }
}
#[test]
fn cpp_fracpack() {
    for (id, bytes) in fixtures("frac32") {
        let encoded = match id.as_str() {
            "record" | "empty_record" => {
                let value = record(&id);
                assert_eq!(Record::unpacked(&bytes).unwrap(), value);
                value.packed()
            }
            "u32" => { assert_eq!(u32::unpacked(&bytes).unwrap(), 0xdeadbeef); 0xdeadbeefu32.packed() }
            "string" => { assert_eq!(String::unpacked(&bytes).unwrap(), "hello"); "hello".to_string().packed() }
            "vector" => { assert_eq!(Vec::<u32>::unpacked(&bytes).unwrap(), vec![1,2,3]); vec![1u32,2,3].packed() }
            "optional_some" => { assert_eq!(Option::<u32>::unpacked(&bytes).unwrap(), Some(42)); Some(42u32).packed() },
            "optional_none" => { assert_eq!(Option::<u32>::unpacked(&bytes).unwrap(), None); None::<u32>.packed() },
            "nested" | "nested_empty" => {
                let value = if id == "nested" { Envelope {
                    head: record("record"), rows: vec![record("empty_record"), Record { id:7, name:"Bob".into(), values:vec![9], active:true }],
                    note: Some("hello".into()), choice: Choice::Text("chosen".into())
                }} else { Envelope { head: record("empty_record"), rows: vec![], note: None, choice: Choice::Number(7) }};
                assert_eq!(Envelope::unpacked(&bytes).unwrap(), value);
                value.packed()
            },
            "string_vector" => {
                let value = vec![String::new(), "hello".into(), "world".into()];
                assert_eq!(Vec::<String>::unpacked(&bytes).unwrap(), value); value.packed()
            },
            "optional_vector" => {
                let value = vec![None, Some(String::new()), Some("hello".into())];
                Vec::<Option<String>>::verify_no_extra(&bytes).unwrap();
                let view = Vec::<Option<String>>::view_validated(&bytes).unwrap();
                assert_eq!(view.get(0), None);
                assert_eq!(view.get(1), Some(""));
                assert_eq!(view.get(2), Some("hello"));
                assert_eq!(view.iter().collect::<Vec<_>>(), vec![None, Some(""), Some("hello")]);
                assert_eq!(Vec::<Option<String>>::unpacked(&bytes).unwrap(), value); value.packed()
            },
            "optional_empty_string" => {
                let value = Some(String::new()); assert_eq!(Option::<String>::unpacked(&bytes).unwrap(), value); value.packed()
            },
            "optional_empty_vector" | "optional_missing_vector" => {
                let value = if id == "optional_empty_vector" { Some(Vec::<u32>::new()) } else { None };
                assert_eq!(Option::<Vec<u32>>::unpacked(&bytes).unwrap(), value); value.packed()
            },
            _ => panic!("Unknown C++ fracpack fixture {id}"),
        };
        assert_eq!(encoded, bytes, "frac32/{id}");
    }
}
#[test]
fn cpp_ssz() {
    for (id, bytes) in fixtures("ssz") {
        let value = record(&id);
        assert_eq!(Record::ssz_unpack(&bytes).unwrap(), value);
        let mut encoded = Vec::new(); value.ssz_pack(&mut encoded);
        assert_eq!(encoded, bytes, "ssz/{id}");
    }
}
#[test]
fn cpp_fracpack_vector_rejection() {
    for (_, bytes) in fixtures("frac32_invalid_strings") {
        assert!(Vec::<String>::unpacked(&bytes).is_err());
        assert!(Vec::<String>::verify_no_extra(&bytes).is_err());
        assert!(Vec::<String>::view_validated(&bytes).is_err());
    }
    for (_, bytes) in fixtures("frac32_invalid_optionals") {
        assert!(Vec::<Option<String>>::unpacked(&bytes).is_err());
        assert!(Vec::<Option<String>>::verify_no_extra(&bytes).is_err());
        assert!(Vec::<Option<String>>::view_validated(&bytes).is_err());
    }
}
#[test]
fn cpp_pssz() {
    use psio::pssz::{Pssz32, to_pssz, from_pssz};
    for (id, bytes) in fixtures("pssz") {
        let value = record(&id);
        assert_eq!(from_pssz::<Pssz32, Record>(&bytes).unwrap(), value);
        assert_eq!(to_pssz::<Pssz32, _>(&value), bytes, "pssz/{id}");
    }
}
#[test]
fn cpp_capnp() {
    for (id, bytes) in fixtures("capnp") {
        let value = record(&id);
        assert_eq!(Record::capnp_unpack(&bytes).unwrap(), value);
        assert_eq!(value.capnp_pack(), bytes, "capnp/{id}");
    }
}
#[test]
fn cpp_flatbuf() {
    for (id, bytes) in fixtures("flatbuf") {
        let value = record(&id);
        assert_eq!(Record::fb_unpack(&bytes).unwrap(), value);
        assert_eq!(value.fb_pack(), bytes, "flatbuf/{id}");
    }
}
#[test]
fn cpp_wit() {
    for (id, bytes) in fixtures("wit") {
        assert_eq!(record(&id).wit_pack(), bytes, "wit/{id}");
    }
}
#[test]
fn cpp_pjson() {
    use psio::pjson::{self, Value};
    for (id, bytes) in fixtures("pjson") {
        let input = match id.as_str() {
            "null" => Value::Null, "true" => Value::Bool(true), "false" => Value::Bool(false),
            "uint_inline" => Value::UInt(5), "uint" => Value::UInt(256), "negative" => Value::NegInt(5),
            "string" => Value::Str(b"hello", 0), "decimal" => Value::Decimal { mantissa: 12345, scale: -2 },
            "float" => Value::Float { width_log2: 3, bits: (1.0f64/7.0).to_bits() as u128 },
            "bytes" => Value::Bytes(&[0,127,255], 0),
            "array" => Value::Array(vec![Value::UInt(1), Value::Str(b"hi",0), Value::Bool(true)]),
            "object" => Value::Object(vec![(b"id",Value::UInt(42)),(b"name",Value::Str(b"Alice",0))]),
            _ => panic!("Unknown C++ pjson fixture {id}"),
        };
        pjson::validate(&bytes).unwrap();
        let mut encoded = Vec::new(); pjson::encode(&input, &mut encoded);
        assert_eq!(encoded, bytes, "pjson/{id}");
        let mut roundtrip = Vec::new();
        pjson::encode(&pjson::decode(&bytes).unwrap(), &mut roundtrip);
        assert_eq!(roundtrip, bytes, "pjson decode/{id}");
    }
}

#[test]
fn cpp_pjson_numbers() {
    for (id, bytes) in fixtures("pjson_f64") {
        let value = f64::from_bits(id.parse().unwrap());
        assert_eq!(psio::pjson::to_pjson(&value), bytes, "pjson double {value} ({id})");
        let decoded: f64 = psio::pjson::from_pjson(&bytes).unwrap();
        if value.is_nan() { assert!(decoded.is_nan()); }
        else { assert_eq!(decoded, value); }
    }
}
#[test]
fn cpp_pjson_containers() {
    for format in ["pjson_extra", "pjson_typed"] {
        for (id, bytes) in fixtures(format) {
            psio::pjson::validate(&bytes).unwrap();
            let value = psio::pjson::decode(&bytes).unwrap();
            let mut output = vec![]; psio::pjson::encode(&value, &mut output);
            assert_eq!(output, bytes, "{format}/{id}");
        }
    }
}
#[test]
fn pjson_integer_and_depth_boundaries() {
    use psio::pjson::{to_pjson, from_pjson, encode, validate, decode, Value};
    for value in [i128::MIN, -1, 0, i128::MAX] {
        assert_eq!(from_pjson::<i128>(&to_pjson(&value)).unwrap(), value);
    }
    assert!(from_pjson::<i8>(&to_pjson(&-129i16)).is_err());
    assert!(from_pjson::<i128>(&to_pjson(&u128::MAX)).is_err());
    for tag in 0xa1..=0xaf { assert!(validate(&[tag, 0]).is_err()); }
    let mut nested = vec![0];
    // Construct bytes iteratively, so the test itself never recurses deeply.
    for _ in 0..1000 {
        let code = if nested.len() <= 255 { 0 } else if nested.len() <= 65535 { 1 } else { 2 };
        let mut outer = vec![0xb0, code]; outer.extend(nested);
        outer.resize(outer.len() + code as usize + 1, 0); outer.extend([1,0]);
        nested = outer;
    }
    assert!(validate(&nested).is_err()); assert!(decode(&nested).is_err());
    let mut single = vec![]; encode(&Value::Array(vec![Value::Null]), &mut single);
    assert!(validate(&single).is_ok());
}

#[test]
fn pjson_revision_two_explicit_tags() {
    use psio::pjson::{self, from_pjson, to_pjson};
    use psio::pjson_view::View as PjsonView;
    assert_eq!(pjson::WIRE_REVISION, 2);
    for n in -15i64..=15 {
        let tag = if n < 0 { 0x30 | (-n as u8) } else { 0x20 | n as u8 };
        assert_eq!(to_pjson(&n), vec![tag]);
        assert_eq!(from_pjson::<i64>(&[tag]).unwrap(), n);
        assert_eq!(PjsonView::new(&[tag]).as_int().unwrap(), n as i128);
        assert_eq!(psio::pjson_json::to_json(&[tag]).unwrap(), n.to_string());
        assert_eq!(psio::pjson_json::from_json(&n.to_string()).unwrap(), vec![tag]);
    }
    assert_eq!(from_pjson::<i64>(&[0x35]).unwrap(), -5);
    for bytes in [&[0x30][..], &[0x35,0], &[0x80,0x25], &[0xd0]] {
        assert!(pjson::validate(bytes).is_err());
        assert!(pjson::decode(bytes).is_err());
    }
}
#[test]
fn cpp_pjson_rejections() {
    for (id, bytes) in fixtures("pjson_invalid") {
        assert!(psio::pjson::validate(&bytes).is_err(), "{id}");
        assert!(psio::pjson::decode(&bytes).is_err(), "{id}");
    }
}

#[test]
fn cpp_pjson_typed_records() {
    use psio::pjson::{to_pjson, from_pjson};
    for (id, bytes) in fixtures("pjson_typed") {
        match id.as_str() {
            "record_rows" | "empty_record_rows" => {
                let rows = if id == "record_rows" { vec![record("record"), record("empty_record")] } else { vec![] };
                let value = Rows { rows };
                assert_eq!(to_pjson(&value), bytes, "{id}");
                assert_eq!(from_pjson::<Rows>(&bytes).unwrap(), value, "{id}");
            }
            "strings" | "empty_strings" => {
                let value = Strings { values: if id == "strings" { vec!["hello".into(), "".into(), "x".repeat(300)] } else { vec![] } };
                assert_eq!(to_pjson(&value), bytes, "{id}");
                assert_eq!(from_pjson::<Strings>(&bytes).unwrap(), value, "{id}");
            }
            "bytes" | "empty_bytes" => {
                let value = Bytes { values: if id == "bytes" { vec![0,127,255] } else { vec![] } };
                assert_eq!(to_pjson(&value), bytes, "{id}");
                assert_eq!(from_pjson::<Bytes>(&bytes).unwrap(), value, "{id}");
            }
            _ => {}
        }
    }
}
