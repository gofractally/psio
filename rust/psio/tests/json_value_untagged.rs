//! Untagged variants, `psio::JsonValue`, and errors that name the member:
//! the Rust twins of `cpp/tests/json_api_tests.cpp` ("an untagged variant is
//! the bare value of its alternative", "untagged variants in a record, in
//! PSSZ and through the schema", the `json_value` cases), typed and
//! schema-driven, with the C++ texts.

use psio::json::{decode, IJson, Json, JsonKind};
use psio::pssz::dynamic::{self, Compare};
use psio::pssz::schema_ir::{
    reserved_custom_id, schema_for, AnyType, AnyValue, Attribute, Float, Int, Member, Schema, Variant,
};
use psio::{JsonNumber, JsonText, JsonValue, PsszPack, PsszUnpack, ToPsszSchema};
use std::collections::BTreeMap;

const NONE_OF: &str = "json: the value is none of the untagged variant's alternatives";

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema, JsonText)]
#[psio(untagged)]
enum Extra {
    Text(String),
    List(Vec<String>),
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema, JsonText)]
struct Point {
    x: i32,
    y: i32,
}

// serde's spelling is honoured: the alternatives are of five different kinds.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema, JsonText, psio::Serde)]
#[serde(untagged)]
enum Mixed {
    Flag(bool),
    Count(i32),
    Text(String),
    List(Vec<i32>),
    Point(Point),
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema, JsonText)]
struct Config {
    extra: BTreeMap<String, Extra>,
    one: Option<Extra>,
    mixed: Vec<Mixed>,
}

// The same variant, tagged.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema, JsonText)]
enum Tagged {
    Text(String),
    List(Vec<String>),
}

fn what<T: std::fmt::Debug>(result: Result<T, psio::json::JsonTextError>) -> String {
    result.unwrap_err().what.into_owned()
}

#[test]
fn an_untagged_variant_is_the_bare_value_of_its_alternative() {
    let list = Extra::List(vec!["a".into()]);
    assert_eq!(psio::to_ijson(&list), r#"["a"]"#);
    assert_eq!(psio::to_ijson(&Extra::Text("s".into())), r#""s""#);
    assert_eq!(psio::to_json(&Extra::List(vec!["a".into(), "b".into()])), r#"["a","b"]"#);
    assert_eq!(psio::to_json(&Extra::Text("s".into())), r#""s""#);
    assert_eq!(decode::<IJson, Extra>(r#"["a"]"#).unwrap(), list);
    assert_eq!(decode::<Json, Extra>(r#" "x" "#).unwrap(), Extra::Text("x".into()));
    assert_eq!(decode::<IJson, Extra>("[]").unwrap(), Extra::List(vec![]));
    // A value of no alternative's kind; a value of the right kind that is
    // not the alternative.
    assert_eq!(what(decode::<IJson, Extra>("7")), NONE_OF);
    assert!(decode::<IJson, Extra>("[1]").is_err());
    assert!(decode::<IJson, Extra>(r#"{"Text":"x"}"#).is_err());
    assert!(decode::<Json, Extra>(r#"[0,"x"]"#).is_err());
    // The tagged twin keeps its two forms.
    assert_eq!(psio::to_ijson(&Tagged::Text("x".into())), r#"{"Text":"x"}"#);
    assert_eq!(psio::to_json(&Tagged::Text("x".into())), r#"[0,"x"]"#);

    // Five kinds, five alternatives.
    let all = vec![
        Mixed::Flag(true),
        Mixed::Count(7),
        Mixed::Text("s".into()),
        Mixed::List(vec![1, 2]),
        Mixed::Point(Point { x: 3, y: 4 }),
    ];
    let text = r#"[true,7,"s",[1,2],{"x":3,"y":4}]"#;
    assert_eq!(psio::to_ijson(&all), text);
    assert_eq!(psio::to_json(&all), text);
    assert_eq!(decode::<IJson, Vec<Mixed>>(text).unwrap(), all);
    assert_eq!(decode::<Json, Vec<Mixed>>(text).unwrap(), all);
    assert!(decode::<IJson, Mixed>("null").is_err());
    assert!(decode::<IJson, Mixed>("1.5").is_err()); // a number: the i32
    // The kinds the types say they are written as.
    use psio::json::kinds;
    assert_eq!(<Extra as psio::json::JsonText>::JSON_KINDS, kinds::STRING | kinds::ARRAY);
    assert_eq!(
        <Mixed as psio::json::JsonText>::JSON_KINDS,
        kinds::BOOLEAN | kinds::NUMBER | kinds::STRING | kinds::ARRAY | kinds::OBJECT
    );
    assert_eq!(<Tagged as psio::json::JsonText>::JSON_KINDS, kinds::ARRAY | kinds::OBJECT);
    assert_eq!(<Option<Extra> as psio::json::JsonText>::JSON_KINDS, kinds::NULL | kinds::STRING | kinds::ARRAY);
    assert_eq!(<Vec<u8> as psio::json::JsonText>::JSON_KINDS, kinds::ARRAY | kinds::STRING);
    assert_eq!(<u64 as psio::json::JsonText>::JSON_KINDS, kinds::NUMBER | kinds::STRING);
    assert_eq!(<u32 as psio::json::JsonText>::JSON_KINDS, kinds::NUMBER);
    // Through serde (psio::Serde reads and writes the type's own text).
    assert_eq!(serde_json::to_string(&all).unwrap(), text);
    assert_eq!(serde_json::from_str::<Vec<Mixed>>(text).unwrap(), all);
}

fn untagged_nodes(schema: &Schema) -> usize {
    schema
        .types
        .iter()
        .filter(|named| matches!(&named.r#type.value,
            AnyValue::Variant(v) if v.attributes.iter().any(|a| a.name == "untagged")))
        .count()
}

#[test]
fn untagged_variants_in_a_record_in_pssz_and_through_the_schema() {
    let value = Config {
        extra: BTreeMap::from([
            ("hosts".to_string(), Extra::List(vec!["a".into(), "b".into()])),
            ("name".to_string(), Extra::Text("n".into())),
        ]),
        one: Some(Extra::Text("one".into())),
        mixed: vec![Mixed::Point(Point { x: 1, y: 2 }), Mixed::Flag(false)],
    };
    let text = r#"{"extra":{"hosts":["a","b"],"name":"n"},"one":"one","mixed":[{"x":1,"y":2},false]}"#;
    assert_eq!(psio::to_ijson(&value), text);
    assert_eq!(psio::to_json(&value), text);
    assert_eq!(decode::<IJson, Config>(text).unwrap(), value);
    // An optional untagged variant: null is none.
    let back = decode::<IJson, Config>(r#"{"extra":{},"one":null,"mixed":[]}"#).unwrap();
    assert_eq!(back.one, None);

    // Binary forms are untouched: the selector byte, then the alternative.
    let one = psio::to_pssz(&Extra::List(vec!["a".into()]));
    assert_eq!(one[0], 1);
    assert_eq!(one, psio::to_pssz(&Tagged::List(vec!["a".into()])));

    // The schema says `untagged`, and the schema-driven codecs follow.
    let (schema, root) = schema_for::<Config>().unwrap();
    dynamic::validate_schema(&schema).unwrap();
    assert_eq!(untagged_nodes(&schema), 2);
    let bytes = psio::to_pssz(&value);
    assert_eq!(dynamic::pssz_to_json::<IJson>(&schema, &root, &bytes).unwrap().0, text);
    assert_eq!(dynamic::pssz_to_json::<Json>(&schema, &root, &bytes).unwrap().0, text);
    assert_eq!(dynamic::json_to_pssz::<IJson>(&schema, &root, text.as_bytes()).unwrap(), bytes);
    assert_eq!(dynamic::json_to_pssz::<Json>(&schema, &root, text.as_bytes()).unwrap(), bytes);
    let error = dynamic::json_to_pssz::<IJson>(&schema, &root, br#"{"extra":{"k":7},"one":null,"mixed":[]}"#)
        .unwrap_err();
    assert_eq!(error.what(), NONE_OF);

    // A schema whose alternatives cannot be told apart is refused where a
    // value is read.
    let member = |name: &str, value: AnyValue| Member {
        name: name.into(),
        r#type: Box::new(AnyType::new(value)),
        attributes: vec![],
    };
    let mut ambiguous = Schema::default();
    ambiguous.types.push(member(
        "V",
        AnyValue::Variant(Variant {
            members: vec![
                member("0", AnyValue::Int(Int { bits: 32, isSigned: false })),
                member("1", AnyValue::Float(Float { exp: 11, mantissa: 53 })),
            ],
            attributes: vec![Attribute { name: "untagged".into(), value: Some(String::new()) }],
        }),
    ));
    dynamic::validate_schema(&ambiguous).unwrap();
    let error = dynamic::json_to_pssz::<IJson>(&ambiguous, &AnyType::named("V"), b"1").unwrap_err();
    assert_eq!(
        error.what(),
        "json: the alternatives of an untagged variant are not told apart by the kind of JSON value"
    );

    // Whether a variant is untagged is part of its text contract.
    let (bare, bare_root) = schema_for::<Extra>().unwrap();
    let (tagged, tagged_root) = schema_for::<Tagged>().unwrap();
    assert!(dynamic::equivalent(&tagged, &tagged_root, &bare, &bare_root, Compare::Wire).ok);
    let contract = dynamic::equivalent(&tagged, &tagged_root, &bare, &bare_root, Compare::Contract);
    assert_eq!(contract.code, "text-form-changed");
}

// (Two alternatives written as the same kind do not compile: the
// `compile_fail` examples of `psio::json::untagged_kinds`.)

// ── psio::JsonValue ─────────────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema, JsonText)]
struct Pretty {
    service: String,
    data: JsonValue,
    more: Option<JsonValue>,
    list: Vec<JsonValue>,
}
/// The same bytes: each json value is a UTF-8 string.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema, JsonText)]
struct PrettyText {
    service: String,
    data: String,
    more: Option<String>,
    list: Vec<String>,
}

#[test]
fn a_document_read_and_written_again_is_the_same_text() {
    for text in [
        "null",
        "true",
        "false",
        "0",
        "-0",
        "1.50",
        "1e3",
        "-12.5e-3",
        "18446744073709551616",
        r#""""#,
        r#""a\"b\\c\n""#,
        "[]",
        "{}",
        r#"[1,"a",[true,null],{"k":[]}]"#,
        r#"{"a":1.50,"b":[true,null],"c":{"d":"e"}}"#,
    ] {
        let v: JsonValue = psio::from_ijson(text).unwrap();
        assert_eq!(psio::to_ijson(&v), text, "{text}");
        assert_eq!(psio::to_json(&psio::from_json::<JsonValue>(text).unwrap()), text, "{text}");
    }
    assert_eq!(
        psio::from_ijson::<JsonValue>(" [ 1 , \"a\" ] ").unwrap(),
        JsonValue::Array(vec![1.into(), "a".into()])
    );
    for bad in ["", "{", "[1,]", "nul", "01", "1.", r#"{"a"}"#, "1 2", "\"\\x\""] {
        assert!(psio::from_ijson::<JsonValue>(bad).is_err(), "{bad}");
        assert!(psio::validate_ijson::<JsonValue>(bad).is_err(), "{bad}");
    }
    // Nesting: at most 64 levels, as every reader.
    let deep = format!("{}{}", "[".repeat(65), "]".repeat(65));
    assert!(psio::from_ijson::<JsonValue>(&deep).is_err());
    let fits = format!("{}{}", "[".repeat(64), "]".repeat(64));
    assert!(psio::from_ijson::<JsonValue>(&fits).is_ok());
}

#[test]
fn a_repeated_key_keeps_its_last_value_at_the_first_position() {
    let v: JsonValue = psio::from_ijson(r#"{"a":1,"b":2,"a":{"x":[]},"b":3}"#).unwrap();
    assert_eq!(psio::to_ijson(&v), r#"{"a":{"x":[]},"b":3}"#);
    let built = JsonValue::Object(vec![("k".into(), 1.into()), ("j".into(), 2.into()), ("k".into(), "last".into())]);
    assert_eq!(psio::to_ijson(&built), r#"{"k":"last","j":2}"#);
    assert_eq!(built.find("k").and_then(JsonValue::as_str), Some("last"));
}

#[test]
fn built_and_edited_in_code() {
    let mut v = JsonValue::default();
    assert!(v.is_null());
    v.set("name", "x").set("n", 7).set("flag", true).set("none", None::<bool>);
    v.set("list", vec![JsonValue::from(1), JsonValue::from(-2), JsonValue::from("three")]);
    v.set("n", 8);
    assert!(v.erase("flag"));
    assert!(!v.erase("flag"));
    assert_eq!(psio::to_ijson(&v), r#"{"name":"x","n":8,"none":null,"list":[1,-2,"three"]}"#);
    assert_eq!(v.kind(), JsonKind::Object);
    assert_eq!(v.find("n").and_then(JsonValue::as_number).and_then(JsonNumber::as_i64), Some(8));
    assert_eq!(v.find("list").and_then(JsonValue::as_array).map(<[JsonValue]>::len), Some(3));
    assert_eq!(v.find("missing"), None);
    // A number is its text; a JsonValue does not know its width, so ijson
    // cannot give it the string form of a 64-bit integer.
    assert_eq!(psio::to_ijson(&JsonValue::from(u64::MAX)), "18446744073709551615");
    assert_eq!(JsonNumber::new("1.50").unwrap().as_str(), "1.50");
    assert_eq!(JsonNumber::new("1.50").unwrap().as_f64(), Some(1.5));
    assert_eq!(JsonNumber::new("1.50").unwrap().as_u64(), None);
    for bad in ["0x10", "", "1.", "+1", "NaN", "1 ", "01"] {
        assert!(JsonNumber::new(bad).is_none(), "{bad}");
    }
    assert_eq!(JsonNumber::try_from(1.5f64).unwrap().as_str(), "1.5");
    assert!(JsonNumber::try_from(f64::INFINITY).is_err());
    // Under ijson a string holding a Unicode noncharacter is not written.
    let nonchar = JsonValue::from("\u{FFFF}");
    assert!(psio::try_to_ijson(&nonchar).is_err());
    assert!(psio::try_to_json(&nonchar).is_ok());
    // serde_json::Value, through the text.
    let serde = v.to_serde_json().unwrap();
    assert_eq!(serde["list"][2], "three");
    assert_eq!(JsonValue::from(&serde).find("n"), v.find("n"));
    assert_eq!(serde_json::to_string(&v).unwrap(), psio::to_ijson(&v));
    assert_eq!(serde_json::from_str::<JsonValue>(r#"{"a":[1.50]}"#).unwrap(), psio::from_ijson(r#"{"a":[1.50]}"#).unwrap());
}

#[test]
fn a_member_of_a_record_in_text_and_in_pssz() {
    let text = r#"{"service":"tokens","data":{"amount":"5","memo":[1.0,null]},"more":7,"list":[true,"x",{}]}"#;
    let p: Pretty = psio::from_ijson(text).unwrap();
    assert_eq!(psio::to_ijson(&p), text);
    assert_eq!(p.data.find("amount").and_then(JsonValue::as_str), Some("5"));
    // A missing optional member is none; a missing JsonValue is an error
    // (null is a value: it has to be written).
    let q: Pretty = psio::from_ijson(r#"{"service":"s","data":null,"list":[]}"#).unwrap();
    assert!(q.data.is_null());
    assert_eq!(q.more, None);
    assert_eq!(
        what(psio::from_ijson::<Pretty>(r#"{"service":"s","list":[]}"#)),
        r#"json: missing required member "data" of Pretty"#
    );

    // PSSZ: each json value is a UTF-8 string holding its compact text.
    let twin = PrettyText {
        service: "tokens".into(),
        data: r#"{"amount":"5","memo":[1.0,null]}"#.into(),
        more: Some("7".into()),
        list: vec!["true".into(), r#""x""#.into(), "{}".into()],
    };
    let bytes = psio::to_pssz(&p);
    assert_eq!(bytes, psio::to_pssz(&twin));
    psio::validate_pssz::<Pretty>(&bytes).unwrap();
    assert_eq!(psio::to_ijson(&psio::from_pssz::<Pretty>(&bytes).unwrap()), text);
    // Bytes that are not JSON text are not a value; text that is JSON but
    // not compact is a value with another, canonical, encoding.
    let bad = PrettyText { data: "{not json".into(), ..twin.clone() };
    let bad_bytes = psio::to_pssz(&bad);
    assert!(psio::validate_pssz::<Pretty>(&bad_bytes).is_err());
    assert!(psio::from_pssz::<Pretty>(&bad_bytes).is_err());
    let loose = PrettyText { data: r#"{ "amount" : "5" }"#.into(), ..twin.clone() };
    let loose_bytes = psio::to_pssz(&loose);
    psio::validate_pssz::<Pretty>(&loose_bytes).unwrap();
    assert_eq!(
        psio::to_pssz(&psio::from_pssz::<Pretty>(&loose_bytes).unwrap()),
        psio::to_pssz(&PrettyText { data: r#"{"amount":"5"}"#.into(), ..twin.clone() })
    );

    // The schema: Custom "json" over a UTF-8 string; and the schema-driven
    // codecs agree with the typed ones on bytes and on text.
    let (schema, root) = schema_for::<Pretty>().unwrap();
    dynamic::validate_schema(&schema).unwrap();
    let AnyValue::Object(record) = &schema.resolve(&root).unwrap().value else { panic!("a record") };
    let AnyValue::Custom(custom) = &schema.resolve(&record.members[1].r#type).unwrap().value else {
        panic!("a custom node")
    };
    assert_eq!(custom.id, "json");
    assert!(matches!(&schema.resolve(&custom.r#type).unwrap().value, AnyValue::Custom(c) if c.id == "utf8"));
    assert_eq!(dynamic::pssz_to_json::<IJson>(&schema, &root, &bytes).unwrap().0, psio::to_ijson(&p));
    assert_eq!(dynamic::pssz_to_json::<Json>(&schema, &root, &bytes).unwrap().0, psio::to_json(&p));
    assert_eq!(dynamic::json_to_pssz::<IJson>(&schema, &root, psio::to_ijson(&p).as_bytes()).unwrap(), bytes);
    assert_eq!(dynamic::json_to_pssz::<Json>(&schema, &root, psio::to_json(&p).as_bytes()).unwrap(), bytes);
    assert!(dynamic::validate_pssz(&schema, &root, &bytes).is_ok());
    assert!(dynamic::validate_pssz(&schema, &root, &bad_bytes).is_err());
    assert!(dynamic::pssz_to_json::<IJson>(&schema, &root, &bad_bytes).is_err());
    // Loose text in the bytes is presented compact.
    assert_eq!(
        dynamic::pssz_to_json::<IJson>(&schema, &root, &loose_bytes).unwrap().0,
        r#"{"service":"tokens","data":{"amount":"5"},"more":7,"list":[true,"x",{}]}"#
    );
    // The schema-driven reader names the missing member as the schema does.
    assert_eq!(
        dynamic::json_to_pssz::<IJson>(&schema, &root, br#"{"service":"s","list":[]}"#).unwrap_err().what(),
        r#"json: missing required member "data""#
    );
    // A number keeps its digits through the schema-driven path.
    let digits = r#"{"service":"s","data":[1.50,18446744073709551616,-0],"more":null,"list":[]}"#;
    let packed = dynamic::json_to_pssz::<IJson>(&schema, &root, digits.as_bytes()).unwrap();
    assert_eq!(dynamic::pssz_to_json::<IJson>(&schema, &root, &packed).unwrap().0, digits);
    assert_eq!(packed, psio::to_pssz(&psio::from_ijson::<Pretty>(digits).unwrap()));

    // The id is reserved; a json node sits directly on a UTF-8 string.
    assert!(reserved_custom_id("json"));
    let mut wrong = Schema::default();
    wrong.types.push(Member {
        name: "J".into(),
        r#type: Box::new(AnyType::new(AnyValue::Custom(psio::pssz::schema_ir::Custom {
            r#type: Box::new(AnyType::new(AnyValue::Int(Int { bits: 32, isSigned: false }))),
            id: "json".into(),
        }))),
        attributes: vec![],
    });
    assert!(dynamic::validate_schema(&wrong).is_err());
    // As a root, and as the JSON of a schema-only value.
    let (schema, root) = schema_for::<JsonValue>().unwrap();
    let bytes = dynamic::json_to_pssz::<Json>(&schema, &root, br#" { "a" : [ 1 , 2 ] } "#).unwrap();
    assert_eq!(bytes, br#"{"a":[1,2]}"#);
    assert_eq!(bytes, psio::to_pssz(&psio::from_json::<JsonValue>(r#"{"a":[1,2]}"#).unwrap()));
}

#[test]
fn a_missing_member_is_named_with_its_record() {
    #[derive(Debug, PartialEq, JsonText)]
    struct Ev {
        event: String,
        at: Option<u32>,
    }
    assert_eq!(what(psio::from_ijson::<Ev>(r#"{"at":1}"#)), r#"json: missing required member "event" of Ev"#);
    assert_eq!(what(psio::from_json::<Ev>("{}")), r#"json: missing required member "event" of Ev"#);
    assert_eq!(psio::from_ijson::<Ev>(r#"{"event":"e"}"#).unwrap(), Ev { event: "e".into(), at: None });
}
