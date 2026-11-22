//! Cross-language Schema IR text conformance: consumes
//! conformance/schema-json-vectors.json, the fixture emitted from the C++
//! implementation (cpp/tests/schema_json_conformance_emit.cpp) and also
//! checked by ctest and the JS tests.
//!
//! A `Schema` has three serialized forms: its PSSZ bytes and the two texts
//! of `psio::json`. The texts are what the generic `JsonText` codec gives
//! the IR's own records (docs/json.md, "The Schema IR as JSON").
//!
//! * `schemas`: bytes -> `Schema` -> text is exactly `json` / `ijson`;
//!   either text -> `Schema` -> bytes is exactly `ir_hex`.
//! * `accepts`: `text` is not what a writer produces but reads, under
//!   `format`, as the schema whose bytes are `ir_hex`.
//! * `rejects`: `text` is not a schema under `format`.

use psio::json::{self, IJson, Json, JsonFormat};
use psio::pssz::schema_ir::{AnyValue, Schema};
use psio::pssz::Pssz;
use std::collections::BTreeSet;

#[derive(serde::Deserialize)]
struct SchemaRow {
    name: String,
    ir_hex: String,
    json: String,
    ijson: String,
}
#[derive(serde::Deserialize)]
struct AcceptRow {
    name: String,
    format: String,
    text: String,
    ir_hex: String,
}
#[derive(serde::Deserialize)]
struct RejectRow {
    name: String,
    format: String,
    text: String,
}
#[derive(serde::Deserialize)]
struct Fixture {
    version: u32,
    schemas: Vec<SchemaRow>,
    accepts: Vec<AcceptRow>,
    rejects: Vec<RejectRow>,
}

fn fixture() -> Fixture {
    let path = format!("{}/../../conformance/schema-json-vectors.json", env!("CARGO_MANIFEST_DIR"));
    let text = std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{path}: {e}"));
    serde_json::from_str(&text).expect("fixture JSON")
}

fn unhex(text: &str) -> Vec<u8> {
    assert!(text.len() % 2 == 0, "odd hex");
    (0..text.len()).step_by(2).map(|i| u8::from_str_radix(&text[i..i + 2], 16).expect("hex")).collect()
}
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn from_bytes(ir_hex: &str) -> Schema {
    let bytes = unhex(ir_hex);
    psio::pssz::validate::<Pssz, Schema>(&bytes).expect("ir_hex validates");
    psio::pssz::decode_strict::<Pssz, Schema>(&bytes).expect("ir_hex decodes")
}
fn to_hex(schema: &Schema) -> String {
    hex(&psio::pssz::try_encode::<Pssz, Schema>(schema).expect("schema encodes"))
}

/// The schema `text` reads as under `F`, through every reader entry point;
/// they must agree on whether it is one.
fn read<F: JsonFormat>(text: &str) -> Option<Schema> {
    let read = json::decode::<F, Schema>(text).ok();
    assert_eq!(json::decode::<F, Schema>(text.as_bytes()).ok(), read, "from_slice: {text}");
    assert_eq!(json::validate::<F, Schema>(text.as_bytes()).is_ok(), read.is_some(), "validate: {text}");
    read
}

fn check_text<F: JsonFormat>(name: &str, schema: &Schema, ir_hex: &str, text: &str) {
    assert_eq!(json::try_encode::<F, _>(schema).expect("schema has text"), text, "{name}: text");
    assert_eq!(json::try_encode::<F, _>(schema).map(String::into_bytes).expect("schema has text"), text.as_bytes(), "{name}: to_vec");
    let back = read::<F>(text).unwrap_or_else(|| panic!("{name}: the text does not read"));
    assert_eq!(&back, schema, "{name}: value read from text");
    assert_eq!(to_hex(&back), ir_hex, "{name}: bytes of the value read from text");
}

fn by_format<R>(format: &str, run: impl Fn(bool) -> R) -> R {
    match format {
        "json" => run(false),
        "ijson" => run(true),
        other => panic!("unknown format {other}"),
    }
}

#[test]
fn schemas_have_the_recorded_bytes_and_texts() {
    let fx = fixture();
    assert_eq!(fx.version, 1);
    assert!(fx.schemas.len() >= 7);
    let mut names = BTreeSet::new();
    for row in &fx.schemas {
        assert!(names.insert(row.name.as_str()), "duplicate row {}", row.name);
        let schema = from_bytes(&row.ir_hex);
        assert_eq!(to_hex(&schema), row.ir_hex, "{}: PSSZ round trip", row.name);
        check_text::<Json>(&row.name, &schema, &row.ir_hex, &row.json);
        check_text::<IJson>(&row.name, &schema, &row.ir_hex, &row.ijson);
    }
    for name in ["pssz-vectors", "json-vectors", "time-key-vectors", "schema-of-schema", "every-alternative"] {
        assert!(names.contains(name), "missing row {name}");
    }
}

#[test]
fn the_hand_built_schema_holds_every_alternative() {
    let fx = fixture();
    let row = fx.schemas.iter().find(|r| r.name == "every-alternative").expect("every-alternative row");
    let schema = from_bytes(&row.ir_hex);
    // The alternative each named type is, by its I-JSON name: all 15.
    let seen: BTreeSet<String> = schema
        .types
        .iter()
        .map(|m| {
            let text = psio::to_ijson(&m.r#type.value);
            text.split('"').nth(1).unwrap().to_string()
        })
        .collect();
    assert_eq!(seen.len(), 15, "{seen:?}");
    assert_eq!(schema.ir_version, None);
    assert!(!schema.interfaces.is_empty() && !schema.worlds.is_empty() && !schema.uses.is_empty());
    assert!(!schema.package.attributes.is_empty());
    // A 64-bit integer beyond 2^53 - 1: bare in json, a string in ijson.
    assert!(row.json.contains(r#""len":18446744073709551615"#));
    assert!(row.ijson.contains(r#""len":"18446744073709551615""#));
    assert!(row.ijson.contains(r#""len":9007199254740991"#));
    assert!(row.ijson.contains(r#""len":"9007199254740992""#));
}

#[test]
fn accepted_texts_read_as_the_recorded_schema() {
    let fx = fixture();
    assert!(fx.accepts.len() >= 18);
    for row in &fx.accepts {
        let read = by_format(&row.format, |ijson| if ijson { read::<IJson>(&row.text) } else { read::<Json>(&row.text) });
        let schema = read.unwrap_or_else(|| panic!("{}: rejected: {}", row.name, row.text));
        assert_eq!(to_hex(&schema), row.ir_hex, "{}", row.name);
    }
}

#[test]
fn rejected_texts_are_rejected() {
    let fx = fixture();
    assert!(fx.rejects.len() >= 40);
    for row in &fx.rejects {
        let read = by_format(&row.format, |ijson| if ijson { read::<IJson>(&row.text) } else { read::<Json>(&row.text) });
        assert!(read.is_none(), "{}: accepted: {}", row.name, row.text);
    }
}

#[test]
fn a_box_is_its_value_in_text() {
    use psio::pssz::schema_ir::{AnyType, Int};
    let inner = AnyType::new(AnyValue::Int(Int { bits: 8, isSigned: false }));
    let boxed = Box::new(inner.clone());
    assert_eq!(psio::to_ijson(&boxed), psio::to_ijson(&inner));
    assert_eq!(psio::to_json(&boxed), r#"{"value":[8,{"bits":8,"isSigned":false}]}"#);
    assert_eq!(psio::to_ijson(&boxed), r#"{"value":{"Int":{"bits":8,"isSigned":false}}}"#);
    assert_eq!(psio::from_ijson::<Box<AnyType>>(r#"{"value":{"Int":{"bits":8,"isSigned":false}}}"#).unwrap(), boxed);
}

// ── The schema of the Schema IR (RG5) ───────────────────────────────────────

/// The IR's own records describe themselves: `schema_for::<Schema>()` is
/// the fixture's `schema-of-schema`, which C++ writes with
/// `SchemaBuilder{}.insert<psio::Schema>()`.
#[test]
fn the_schema_of_the_schema_ir_is_the_cpp_one() {
    let fixture = fixture();
    let row = fixture.schemas.iter().find(|s| s.name == "schema-of-schema").expect("the fixture row");
    let (schema, root) = psio::pssz::schema_ir::schema_for::<Schema>().unwrap();
    assert_eq!(root, psio::pssz::schema_ir::AnyType::named("Schema"));
    let expected = from_bytes(&row.ir_hex);
    let names = |s: &Schema| s.types.iter().map(|m| m.name.clone()).collect::<Vec<_>>();
    assert_eq!(names(&schema), names(&expected));
    for (ours, theirs) in schema.types.iter().zip(&expected.types) {
        assert_eq!(ours, theirs, "type {}", ours.name);
    }
    assert_eq!(schema, expected);
    assert_eq!(to_hex(&schema), row.ir_hex);
    assert_eq!(psio::to_ijson(&schema), row.ijson);
}

/// A schema is a value: a member of a record, an argument of a method.
#[test]
fn a_schema_is_a_member_and_an_argument() {
    use psio::{JsonText, PsszPack, PsszUnpack, ToPsszSchema};

    #[derive(Debug, PartialEq, PsszPack, PsszUnpack, ToPsszSchema, JsonText)]
    struct Published {
        service: String,
        schema: Schema,
        root: psio::pssz::schema_ir::AnyType,
    }
    let (schema, root) = psio::pssz::schema_ir::schema_for::<(u32, String)>().unwrap();
    let value = Published { service: "tokens".into(), schema, root };
    let bytes = psio::to_pssz(&value);
    assert_eq!(psio::from_pssz::<Published>(&bytes).unwrap(), value);
    assert_eq!(psio::from_ijson::<Published>(&psio::to_ijson(&value)).unwrap(), value);

    // Its schema names the IR's records, and the schema-driven codec reads
    // the value with it.
    let (described, described_root) = psio::pssz::schema_ir::schema_for::<Published>().unwrap();
    assert!(described.get("Schema").is_some() && described.get("AnyType").is_some());
    psio::pssz::dynamic::validate(&described, &described_root, &bytes).unwrap();
    let (text, _) =
        psio::pssz::dynamic::json::pssz_to_json::<psio::IJson>(&described, &described_root, &bytes).unwrap();
    assert_eq!(text, psio::to_ijson(&value));
}
