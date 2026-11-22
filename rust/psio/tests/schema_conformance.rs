//! Schema-driven conformance: a consumer that holds ONLY a Schema IR (no Rust
//! type) must agree with the typed codecs. This test reads the two shared
//! fixtures and uses nothing but the IR recorded in them (the IR the C++
//! `SchemaBuilder` produced, decoded with the typed Rust `Pssz` codec):
//!
//!   conformance/json-vectors.json   IR + text  -> the typed psio::pssz bytes
//!                                   IR + bytes -> the typed json / ijson text
//!                                   IR + rejected text -> rejected
//!   conformance/pssz-vectors.json   IR + bytes -> the typed validate outcome
//!                                   (canonical / unknown / reject), the
//!                                   re-encoding, and a JSON round trip
//!
//! The C++ counterpart, test for test, is
//! cpp/tests/schema_conformance_tests.cpp. (The IR Rust generates for its own
//! types is checked against the typed Rust codecs in json_conformance.rs.)

use psio::json::{IJson, Json, JsonFormat};
use psio::pssz::dynamic::{self, Value};
use psio::pssz::schema_ir::{AnyType, Schema};
use psio::pssz::{from_pssz, pssz_validate, Pssz, Pssz16, Pssz32, Pssz8, PsszFormat};
use std::collections::{BTreeMap, BTreeSet};

#[derive(Debug, Clone, serde::Deserialize)]
struct TypeRow {
    id: String,
    ir: bool,
    pssz: Option<bool>,
}

#[derive(Debug, Clone, serde::Deserialize)]
struct JsonRow {
    name: String,
    #[serde(rename = "type")]
    ty: String,
    kind: String,
    format: Option<String>,
    json: Option<String>,
    ijson: Option<String>,
    text: Option<String>,
    text_hex: Option<String>,
    canonical: Option<String>,
    pssz: Option<String>,
}

#[derive(Debug, serde::Deserialize)]
struct JsonFixture {
    version: u32,
    schema_ir: String,
    types: Vec<TypeRow>,
    vectors: Vec<JsonRow>,
}

#[derive(Debug, Clone, serde::Deserialize)]
struct PsszRow {
    name: String,
    #[serde(rename = "type")]
    ty: String,
    format: String,
    hex: String,
    expect: String,
    #[serde(default)]
    reencoded: String,
}

#[derive(Debug, serde::Deserialize)]
struct PsszFixture {
    version: u32,
    schema_ir: String,
    types: Vec<TypeRow>,
    vectors: Vec<PsszRow>,
}

fn load<F: serde::de::DeserializeOwned>(file: &str) -> F {
    let path = format!("{}/../../conformance/{file}", env!("CARGO_MANIFEST_DIR"));
    let text = std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{path}: {e}"));
    assert!(!text.is_empty());
    serde_json::from_str(&text).expect("fixture JSON")
}

fn from_hex(hex: &str) -> Vec<u8> {
    assert!(hex.len() % 2 == 0, "odd hex");
    (0..hex.len()).step_by(2).map(|i| u8::from_str_radix(&hex[i..i + 2], 16).expect("hex")).collect()
}

/// The IR travels as its own psio::pssz encoding.
fn schema_of(hex: &str) -> Schema {
    let bytes = from_hex(hex);
    pssz_validate::<Pssz, Schema>(&bytes).expect("schema_ir validates");
    from_pssz::<Pssz, Schema>(&bytes).expect("schema_ir decodes")
}

fn input_text(row: &JsonRow) -> Vec<u8> {
    match (&row.text, &row.text_hex) {
        (Some(text), _) => text.as_bytes().to_vec(),
        (None, Some(hex)) => from_hex(hex),
        _ => panic!("{}: no text", row.name),
    }
}

/// JSON has one null: Some(None) of a nested optional reads back as None (in
/// the typed text codec too), so such a value does not round-trip through
/// text.
fn has_some_none(value: &Value) -> bool {
    match value {
        Value::Optional(Some(inner)) => {
            matches!(**inner, Value::Optional(None)) || has_some_none(inner)
        }
        Value::Sequence(items) => items.iter().any(has_some_none),
        Value::Record(fields) => fields.iter().any(|(_, item)| has_some_none(item)),
        Value::Variant { value, .. } => has_some_none(value),
        _ => false,
    }
}

/// Text -> bytes through the schema: the bytes, or what failed.
fn to_bytes<F: JsonFormat>(schema: &Schema, ty: &str, text: &[u8]) -> Result<Vec<u8>, String> {
    dynamic::json_to_pssz::<F>(schema, &AnyType::named(ty), text).map_err(|e| e.what().to_string())
}
/// Bytes -> text through the schema: the text, or what failed.
fn to_text<F: JsonFormat>(schema: &Schema, ty: &str, bytes: &[u8]) -> Result<String, String> {
    dynamic::pssz_to_json::<F>(schema, &AnyType::named(ty), bytes)
        .map(|(text, _)| text)
        .map_err(|e| e.what().to_string())
}

#[test]
fn schema_ir_and_json_text_agree_with_the_typed_json_ijson_and_pssz_codecs() {
    let fx: JsonFixture = load("json-vectors.json");
    assert_eq!(fx.version, 1);
    let schema = schema_of(&fx.schema_ir);
    let types: BTreeMap<&str, &TypeRow> = fx.types.iter().map(|t| (t.id.as_str(), t)).collect();
    assert!(types.len() >= 40);

    let (mut values, mut accepted, mut rejected, mut with_bytes) = (0usize, 0usize, 0usize, 0usize);
    let mut covered = BTreeSet::new();
    for row in &fx.vectors {
        let ty = types.get(row.ty.as_str()).unwrap_or_else(|| panic!("{}: unlisted type", row.name));
        if !ty.ir {
            continue;
        }
        let name = &row.name;
        covered.insert(row.ty.as_str());
        assert_eq!(
            row.pssz.is_some(),
            ty.pssz.unwrap_or(false) && row.kind != "reject",
            "{name}: pssz presence"
        );
        let root = AnyType::named(row.ty.as_str());
        let ijson = row.format.as_deref() == Some("ijson");
        match row.kind.as_str() {
            "value" => {
                values += 1;
                let json_text = row.json.as_ref().expect("json");
                let ijson_text = row.ijson.as_ref().expect("ijson");
                let bytes = to_bytes::<Json>(&schema, &row.ty, json_text.as_bytes())
                    .unwrap_or_else(|e| panic!("{name}: json -> pssz: {e}"));
                let again = to_bytes::<IJson>(&schema, &row.ty, ijson_text.as_bytes())
                    .unwrap_or_else(|e| panic!("{name}: ijson -> pssz: {e}"));
                assert_eq!(bytes, again, "{name}: json and ijson give different bytes");
                if let Some(hex) = &row.pssz {
                    with_bytes += 1;
                    assert_eq!(bytes, from_hex(hex), "{name}: bytes");
                }
                assert_eq!(dynamic::validate_pssz(&schema, &root, &bytes), Ok(false), "{name}");
                assert_eq!(to_text::<Json>(&schema, &row.ty, &bytes).as_ref(), Ok(json_text), "{name}");
                assert_eq!(
                    to_text::<IJson>(&schema, &row.ty, &bytes).as_ref(),
                    Ok(ijson_text),
                    "{name}"
                );
            }
            "accept" => {
                accepted += 1;
                let text = input_text(row);
                let bytes = if ijson {
                    to_bytes::<IJson>(&schema, &row.ty, &text)
                } else {
                    to_bytes::<Json>(&schema, &row.ty, &text)
                }
                .unwrap_or_else(|e| panic!("{name}: text -> pssz: {e}"));
                if let Some(hex) = &row.pssz {
                    assert_eq!(bytes, from_hex(hex), "{name}: bytes");
                }
                let text = if ijson {
                    to_text::<IJson>(&schema, &row.ty, &bytes)
                } else {
                    to_text::<Json>(&schema, &row.ty, &bytes)
                };
                assert_eq!(text.as_ref(), Ok(row.canonical.as_ref().expect("canonical")), "{name}");
            }
            "reject" => {
                rejected += 1;
                let text = input_text(row);
                let result = if ijson {
                    to_bytes::<IJson>(&schema, &row.ty, &text)
                } else {
                    to_bytes::<Json>(&schema, &row.ty, &text)
                };
                assert!(result.is_err(), "{name}: accepted as {result:02x?}");
            }
            kind => panic!("{name}: unknown kind {kind}"),
        }
    }
    println!("json fixture: values {values}, accepted {accepted}, rejected {rejected}, with pssz bytes {with_bytes}");
    assert!(values >= 600);
    assert!(accepted >= 60);
    assert!(rejected >= 300);
    assert!(with_bytes >= 600);
    // Every expressible type is exercised.
    for (id, ty) in &types {
        if ty.ir {
            assert!(covered.contains(id), "no vector for {id}");
        }
    }
}

#[test]
fn schema_ir_and_pssz_bytes_agree_with_the_typed_pssz_validator() {
    let fx: PsszFixture = load("pssz-vectors.json");
    assert_eq!(fx.version, 1);
    let schema = schema_of(&fx.schema_ir);
    let expressible: BTreeMap<&str, bool> = fx.types.iter().map(|t| (t.id.as_str(), t.ir)).collect();
    assert!(expressible.len() >= 200);

    // One explicit profile (one inherited width): the outcome, or `None`
    // when the profile refuses the schema.
    fn explicit<F: PsszFormat>(
        schema: &Schema,
        root: &AnyType,
        bytes: &[u8],
    ) -> Option<Result<bool, String>> {
        match dynamic::decode_compatible::<F>(schema, root, bytes) {
            Ok(decoded) => Some(Ok(decoded.has_unknown)),
            // The IR codec does not implement caps there and says so.
            Err(e) if e.0 == dynamic::UNSUPPORTED_CONSTRAINT => None,
            Err(e) => Some(Err(e.0.to_string())),
        }
    }

    let (mut canonical, mut unknown, mut rejected, mut raw_non_utf8) = (0usize, 0usize, 0usize, 0usize);
    let (mut json_trips, mut no_json, mut explicit_width) = (0usize, 0usize, 0usize);
    for row in &fx.vectors {
        if !expressible[row.ty.as_str()] {
            continue;
        }
        let name = &row.name;
        let root = AnyType::named(row.ty.as_str());
        let bytes = from_hex(&row.hex);
        let class = name.split('/').next().unwrap_or("");

        if row.format != "auto" {
            if class == "cap-overflow-explicit-nested" {
                continue;
            }
            let outcome = match row.format.as_str() {
                "pssz8" => explicit::<Pssz8>(&schema, &root, &bytes),
                "pssz16" => explicit::<Pssz16>(&schema, &root, &bytes),
                "pssz32" => explicit::<Pssz32>(&schema, &root, &bytes),
                format => panic!("{name}: unknown format {format}"),
            };
            let Some(outcome) = outcome else { continue };
            explicit_width += 1;
            if class == "string-raw-non-utf8" || row.expect == "reject" {
                assert!(outcome.is_err(), "{name}: accepted");
            } else {
                assert_eq!(outcome, Ok(row.expect == "unknown"), "{name}");
            }
            continue;
        }

        let outcome = dynamic::validate_pssz(&schema, &root, &bytes);
        if class == "string-raw-non-utf8" {
            // The IR has one "string" for raw and UTF-8 strings and the
            // codec treats it as UTF-8: these frames, valid for the typed
            // raw string, are rejected through the schema. Pinned so that a
            // change is noticed; see docs/json.md.
            assert_eq!(row.expect, "canonical", "{name}");
            assert!(outcome.is_err(), "{name}: accepted");
            raw_non_utf8 += 1;
            continue;
        }
        if row.expect == "reject" {
            assert!(outcome.is_err(), "{name}: accepted");
            assert!(dynamic::validate(&schema, &root, &bytes).is_err(), "{name}");
            rejected += 1;
            continue;
        }
        assert_eq!(outcome, Ok(row.expect == "unknown"), "{name}");
        assert_eq!(dynamic::validate(&schema, &root, &bytes), Ok(row.expect == "unknown"), "{name}");
        let decoded = dynamic::decode_compatible::<Pssz>(&schema, &root, &bytes)
            .unwrap_or_else(|e| panic!("{name}: decode: {e:?}"));
        assert_eq!(decoded.has_unknown, row.expect == "unknown", "{name}");
        let value = decoded.value;
        let again = dynamic::encode::<Pssz>(&schema, &root, &value)
            .unwrap_or_else(|e| panic!("{name}: encode: {e:?}"));
        if row.expect == "unknown" {
            assert_eq!(again, from_hex(&row.reencoded), "{name}: projection");
            assert!(dynamic::decode_exact::<Pssz>(&schema, &root, &bytes).is_err(), "{name}");
            unknown += 1;
            continue;
        }
        assert_eq!(again, bytes, "{name}: re-encoding");
        assert!(dynamic::decode_exact::<Pssz>(&schema, &root, &bytes).is_ok(), "{name}");
        canonical += 1;

        // Bytes -> text -> bytes, in both text formats.
        let ijson = to_text::<IJson>(&schema, &row.ty, &bytes);
        if ijson.as_ref().is_err_and(|e| e.contains("no JSON")) {
            no_json += 1; // bit types, non-finite floats
            continue;
        }
        let lossy = has_some_none(&value);
        // A string holding a Unicode noncharacter is JSON but not I-JSON.
        if ijson.as_ref().err().map(String::as_str) != Some("ijson: string holds a Unicode noncharacter") {
            let text = ijson.unwrap_or_else(|e| panic!("{name}: pssz -> ijson: {e}"));
            let again = to_bytes::<IJson>(&schema, &row.ty, text.as_bytes())
                .unwrap_or_else(|e| panic!("{name}: ijson {text} -> pssz: {e}"));
            assert_eq!(again == bytes, !lossy, "{name}: ijson round trip of {text}");
        }
        let text = to_text::<Json>(&schema, &row.ty, &bytes)
            .unwrap_or_else(|e| panic!("{name}: pssz -> json: {e}"));
        let again = to_bytes::<Json>(&schema, &row.ty, text.as_bytes())
            .unwrap_or_else(|e| panic!("{name}: json {text} -> pssz: {e}"));
        assert_eq!(again == bytes, !lossy, "{name}: json round trip of {text}");
        json_trips += usize::from(!lossy);
    }
    // Coverage floors (the fixture has 1301 automatic-profile vectors).
    println!(
        "pssz fixture: canonical {canonical}, unknown {unknown}, rejected {rejected}, \
         raw-non-utf8 {raw_non_utf8}, json round trips {json_trips}, no JSON form {no_json}, \
         explicit {explicit_width}"
    );
    assert!(canonical >= 850, "canonical {canonical}");
    assert!(unknown >= 240, "unknown {unknown}");
    assert!(rejected >= 180, "rejected {rejected}");
    assert!(raw_non_utf8 >= 11, "raw_non_utf8 {raw_non_utf8}");
    assert!(json_trips >= 800, "json_trips {json_trips}");
    assert!(no_json >= 1, "no_json {no_json}");
    assert!(explicit_width >= 1000, "explicit_width {explicit_width}");
}

#[test]
fn the_fixture_auto_widths_follow_from_the_ir() {
    // The fixture records auto_pssz_width_v<T> of every type; the IR alone
    // must give the same width for every type that owns a header or table.
    #[derive(serde::Deserialize)]
    struct Info {
        id: String,
        shape: String,
        auto_width: usize,
        ir: bool,
    }
    #[derive(serde::Deserialize)]
    struct Fixture {
        schema_ir: String,
        types: Vec<Info>,
    }
    let fx: Fixture = load("pssz-vectors.json");
    let schema = schema_of(&fx.schema_ir);
    let mut checked = 0usize;
    for info in fx.types.iter().filter(|t| t.ir) {
        let root = AnyType::named(info.id.as_str());
        let width = dynamic::auto_width(&schema, &root)
            .unwrap_or_else(|e| panic!("{}: auto_width: {e:?}", info.id));
        // Records, arrays and tuples own a header or an offset table.
        let owns_offsets = ["record", "dwnc", "array", "tuple"]
            .iter()
            .any(|kind| info.shape.starts_with(kind));
        if owns_offsets {
            assert_eq!(width, info.auto_width, "{} ({})", info.id, info.shape);
            checked += 1;
        }
    }
    println!("pssz fixture: auto widths checked for {checked} types");
    assert!(checked >= 100, "checked {checked}");
}

#[test]
fn the_schema_driven_entry_points_are_checked() {
    let fx: JsonFixture = load("json-vectors.json");
    let schema = schema_of(&fx.schema_ir);

    // A named type is addressed by its name.
    let inner = AnyType::named("Inner");
    let text = r#"{"id":"18446744073709551615","label":"x","flag":true}"#;
    let bytes = dynamic::json_to_pssz::<IJson>(&schema, &inner, text.as_bytes()).unwrap();
    assert_eq!(
        dynamic::pssz_to_json::<IJson>(&schema, &inner, &bytes).unwrap(),
        (text.to_string(), false)
    );
    assert_eq!(
        dynamic::pssz_to_json::<Json>(&schema, &inner, &bytes).unwrap().0,
        r#"{"id":18446744073709551615,"label":"x","flag":true}"#
    );
    // Text <-> Value directly.
    let value = dynamic::json_to_value::<IJson>(&schema, &inner, text.as_bytes()).unwrap();
    assert_eq!(dynamic::value_to_json::<IJson>(&schema, &inner, &value).unwrap(), text);
    assert_eq!(dynamic::encode::<Pssz>(&schema, &inner, &value).unwrap(), bytes);

    // Unknown type names, truncated and mutated bytes: an error, never a
    // crash.
    let missing = AnyType::named("NoSuchType");
    assert!(dynamic::validate_pssz(&schema, &missing, &bytes).is_err());
    assert!(dynamic::json_to_pssz::<Json>(&schema, &missing, b"1").is_err());
    assert!(dynamic::pssz_to_json::<Json>(&schema, &missing, &bytes).is_err());
    assert!(dynamic::auto_width(&schema, &missing).is_err());
    assert!(dynamic::json_to_pssz::<Json>(&schema, &inner, b"").is_err());
    let mut exercised = 0usize;
    for row in &fx.vectors {
        let (Some(pssz), "value") = (&row.pssz, row.kind.as_str()) else { continue };
        let good = from_hex(pssz);
        let root = AnyType::named(row.ty.as_str());
        for cut in 0..good.len().min(48) {
            let _ = dynamic::validate_pssz(&schema, &root, &good[..cut]);
            let mut flipped = good.clone();
            flipped[cut] ^= 0xff;
            let _ = dynamic::pssz_to_json::<IJson>(&schema, &root, &flipped);
            let _ = dynamic::pssz_to_json::<Json>(&schema, &root, &flipped);
            exercised += 1;
        }
        // Truncated text is an error or a shorter value ("12" of "123"),
        // never a crash; so is text with one byte changed.
        let full = row.ijson.as_ref().expect("ijson").as_bytes();
        for cut in 0..full.len().min(48) {
            let _ = dynamic::json_to_pssz::<IJson>(&schema, &root, &full[..cut]);
            let _ = dynamic::json_to_pssz::<Json>(&schema, &root, &full[..cut]);
            let mut changed = full.to_vec();
            changed[cut] ^= 0x10;
            let _ = dynamic::json_to_pssz::<IJson>(&schema, &root, &changed);
            exercised += 1;
        }
    }
    assert!(exercised >= 5000, "exercised {exercised}");

    // Depth bombs in text.
    let tree = AnyType::named("Tree");
    let arrays = "[".repeat(5000);
    assert!(dynamic::json_to_pssz::<Json>(&schema, &tree, arrays.as_bytes()).is_err());
    let objects = "{\"children\":[".repeat(5000);
    assert!(dynamic::json_to_pssz::<IJson>(&schema, &tree, objects.as_bytes()).is_err());
    let unknown = format!("{{\"zzz\":{}", "[".repeat(5000));
    let all_optional = AnyType::named("AllOptional");
    assert!(dynamic::json_to_pssz::<IJson>(&schema, &all_optional, unknown.as_bytes()).is_err());
    // A value nested as deep as the text reader allows is still an error
    // for the byte codec (64 nodes), not a crash.
    let deep = format!("{}{}", "{\"children\":[".repeat(40), "]}".repeat(40));
    assert!(dynamic::json_to_pssz::<Json>(&schema, &tree, deep.as_bytes()).is_err());
    let shallow = format!("{}{}", "{\"children\":[".repeat(20), "]}".repeat(20));
    let bytes = dynamic::json_to_pssz::<Json>(&schema, &tree, shallow.as_bytes()).unwrap();
    assert_eq!(dynamic::pssz_to_json::<Json>(&schema, &tree, &bytes).unwrap().0, shallow);
}
