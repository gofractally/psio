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
    /// The typed `auto_pssz_width_v<T>`: every row of the PSSZ fixture, the
    /// rows with `pssz` of the JSON fixture.
    auto_width: Option<usize>,
    /// JSON fixture: the type holds a custom text form.
    #[serde(default)]
    custom: bool,
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
    /// Value rows of a `custom` type: the text with no handler registered.
    plain_json: Option<String>,
    plain_ijson: Option<String>,
}

/// A schema every schema-driven entry point refuses, whatever the bytes.
#[derive(Debug, Clone, serde::Deserialize)]
struct SchemaRejectRow {
    name: String,
    schema_ir: String,
    root: String,
}
/// A well-formed neighbour of the rejected schemas, with one value.
#[derive(Debug, Clone, serde::Deserialize)]
struct SchemaAcceptRow {
    name: String,
    schema_ir: String,
    root: String,
    pssz: String,
    json: String,
    ijson: String,
}

#[derive(Debug, serde::Deserialize)]
struct JsonFixture {
    version: u32,
    schema_ir: String,
    types: Vec<TypeRow>,
    vectors: Vec<JsonRow>,
    schema_rejects: Vec<SchemaRejectRow>,
    schema_accepts: Vec<SchemaAcceptRow>,
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

/// The handlers of the JSON fixture's custom text forms, written by hand
/// against the PSSZ bytes of their wire types, as a consumer holding only
/// the schema writes them (json_conformance.rs registers the typed
/// `TextForm`s instead).
fn fixture_handlers() -> dynamic::CustomHandlers {
    use psio::pssz::PsszError;
    const ALPHABET: &[u8; 32] = b".12345abcdefghijklmnopqrstuvwxyz";
    let mut handlers = dynamic::CustomHandlers::new();
    // "AccountName" over Int{64}: 8 little-endian bytes; up to 12 characters
    // of 5 bits each, most significant first, with no trailing dot.
    handlers.add(
        "AccountName",
        |bytes: &[u8]| {
            let value = u64::from_le_bytes(bytes.try_into().map_err(|_| PsszError("AccountName: not a u64"))?);
            let mut text: Vec<u8> =
                (0..12).map(|i| ALPHABET[((value >> (5 * (11 - i))) & 31) as usize]).collect();
            while text.last() == Some(&b'.') {
                text.pop();
            }
            String::from_utf8(text).map_err(|_| PsszError("AccountName: not text"))
        },
        |text: &str| {
            const BAD: PsszError = PsszError("AccountName: expected up to 12 of [a-z1-5.]");
            if text.len() > 12 || text.ends_with('.') {
                return Err(BAD);
            }
            let mut value = 0u64;
            for i in 0..12 {
                let digit = match text.as_bytes().get(i) {
                    Some(c) => ALPHABET.iter().position(|a| a == c).ok_or(BAD)? as u64,
                    None => 0,
                };
                value = (value << 5) | digit;
            }
            Ok(value.to_le_bytes().to_vec())
        },
    );
    // "Version" over the record {major: u16, minor: u16}: under the
    // automatic profile a one-byte header (the fixed region's size, 4) and
    // the two little-endian members. Text: "major.minor".
    handlers.add(
        "Version",
        |bytes: &[u8]| match bytes {
            [4, a, b, c, d] => Ok(format!(
                "{}.{}",
                u16::from_le_bytes([*a, *b]),
                u16::from_le_bytes([*c, *d])
            )),
            _ => Err(PsszError("Version: not the record")),
        },
        |text: &str| {
            const BAD: PsszError = PsszError("Version: expected major.minor");
            let part = |digits: &str| -> Result<u16, PsszError> {
                let canonical = !digits.is_empty()
                    && digits.bytes().all(|b| b.is_ascii_digit())
                    && (digits == "0" || !digits.starts_with('0'));
                if !canonical {
                    return Err(BAD);
                }
                digits.parse().map_err(|_| BAD)
            };
            let (major, minor) = text.split_once('.').ok_or(BAD)?;
            let (major, minor) = (part(major)?, part(minor)?);
            let mut bytes = vec![4];
            bytes.extend(major.to_le_bytes());
            bytes.extend(minor.to_le_bytes());
            Ok(bytes)
        },
    );
    handlers
}

thread_local! {
    static HANDLERS: dynamic::CustomHandlers = fixture_handlers();
}

/// Text -> bytes through the schema, with the fixture's handlers: the
/// bytes, or what failed.
fn to_bytes<F: JsonFormat>(schema: &Schema, ty: &str, text: &[u8]) -> Result<Vec<u8>, String> {
    HANDLERS.with(|handlers| {
        dynamic::json_to_pssz_with::<F>(schema, &AnyType::named(ty), text, handlers)
            .map_err(|e| e.what().to_string())
    })
}
/// Bytes -> text through the schema, with the fixture's handlers: the text,
/// or what failed.
fn to_text<F: JsonFormat>(schema: &Schema, ty: &str, bytes: &[u8]) -> Result<String, String> {
    HANDLERS.with(|handlers| {
        dynamic::pssz_to_json_with::<F>(schema, &AnyType::named(ty), bytes, handlers)
            .map(|(text, _)| text)
            .map_err(|e| e.what().to_string())
    })
}
/// The same with no handler registered: a custom text form reads and writes
/// as its wire type.
fn to_bytes_plain<F: JsonFormat>(schema: &Schema, ty: &str, text: &[u8]) -> Result<Vec<u8>, String> {
    dynamic::json_to_pssz::<F>(schema, &AnyType::named(ty), text).map_err(|e| e.what().to_string())
}
fn to_text_plain<F: JsonFormat>(schema: &Schema, ty: &str, bytes: &[u8]) -> Result<String, String> {
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
    let mut custom_values = 0usize;
    // A strict registry with no handlers: bytes -> I-JSON text.
    let strict = |schema: &Schema, ty: &str, bytes: &[u8]| -> Result<String, String> {
        let mut handlers = dynamic::CustomHandlers::new();
        handlers.set_strict(true);
        dynamic::pssz_to_json_with::<IJson>(schema, &AnyType::named(ty), bytes, &handlers)
            .map(|(text, _)| text)
            .map_err(|e| e.what().to_string())
    };
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
                assert_eq!(row.plain_json.is_some(), ty.custom, "{name}: plain_json");
                assert_eq!(row.plain_ijson.is_some(), ty.custom, "{name}: plain_ijson");
                if !ty.custom {
                    // No custom text form: the registry is never consulted.
                    assert_eq!(to_text_plain::<Json>(&schema, &row.ty, &bytes).as_ref(), Ok(json_text), "{name}");
                    assert_eq!(strict(&schema, &row.ty, &bytes).as_ref(), Ok(ijson_text), "{name}");
                    continue;
                }
                // Without handlers: the wire type's own text, which reads
                // back to the same bytes; the typed text is then no value.
                custom_values += 1;
                let plain_json = row.plain_json.as_ref().expect("plain_json");
                let plain_ijson = row.plain_ijson.as_ref().expect("plain_ijson");
                assert_eq!(to_text_plain::<Json>(&schema, &row.ty, &bytes).as_ref(), Ok(plain_json), "{name}");
                assert_eq!(to_text_plain::<IJson>(&schema, &row.ty, &bytes).as_ref(), Ok(plain_ijson), "{name}");
                assert_eq!(to_bytes_plain::<Json>(&schema, &row.ty, plain_json.as_bytes()).as_ref(), Ok(&bytes), "{name}");
                assert_eq!(to_bytes_plain::<IJson>(&schema, &row.ty, plain_ijson.as_bytes()).as_ref(), Ok(&bytes), "{name}");
                assert_ne!(plain_json, json_text, "{name}");
                assert!(to_bytes_plain::<Json>(&schema, &row.ty, json_text.as_bytes()).is_err(), "{name}");
                // With handlers the plain form is not also accepted.
                assert!(to_bytes::<Json>(&schema, &row.ty, plain_json.as_bytes()).is_err(), "{name}");
                assert!(to_bytes::<IJson>(&schema, &row.ty, plain_ijson.as_bytes()).is_err(), "{name}");
                // Strict, no handlers: an error rather than the fallback.
                assert_eq!(
                    strict(&schema, &row.ty, &bytes),
                    Err("pssz dynamic: no handler registered for a custom type".to_string()),
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
    assert!(custom_values >= 15, "custom value rows: {custom_values}");
    assert_eq!(fx.types.iter().filter(|t| t.custom).count(), 4);
    // Every type is expressible, and exercised.
    assert!(fx.types.iter().all(|t| t.ir), "a type has no IR");
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

    // One explicit profile (one inherited width). A type with a
    // maxDynamicData cap is accepted there too (the cap of the top-level
    // type is enforced, as the typed Pssz8/16/32 do): no row is skipped.
    fn explicit<F: PsszFormat>(
        schema: &Schema,
        root: &AnyType,
        bytes: &[u8],
        name: &str,
    ) -> Result<bool, String> {
        let outcome = dynamic::decode_compatible::<F>(schema, root, bytes)
            .map(|decoded| (decoded.has_unknown, decoded.value))
            .map_err(|e| e.0.to_string());
        assert_ne!(
            outcome.as_ref().err().map(String::as_str),
            Some(dynamic::UNSUPPORTED_CONSTRAINT),
            "{name}: the explicit profile refused the schema"
        );
        if let Ok((false, value)) = &outcome {
            // What it accepts with nothing unknown it writes back unchanged
            // (so the top-level cap holds on the way out as well).
            assert_eq!(
                dynamic::encode::<F>(schema, root, value).map_err(|e| e.0.to_string()).as_deref(),
                Ok(bytes),
                "{name}: explicit re-encoding"
            );
            assert!(dynamic::decode_exact::<F>(schema, root, bytes).is_ok(), "{name}");
        }
        outcome.map(|(unknown, _)| unknown)
    }

    let (mut canonical, mut unknown, mut rejected, mut raw_non_utf8) = (0usize, 0usize, 0usize, 0usize);
    let (mut json_trips, mut no_json, mut explicit_width) = (0usize, 0usize, 0usize);
    let (mut explicit_nested_caps, mut explicit_top_caps) = (0usize, 0usize);
    for row in &fx.vectors {
        if !expressible[row.ty.as_str()] {
            continue;
        }
        let name = &row.name;
        let root = AnyType::named(row.ty.as_str());
        let bytes = from_hex(&row.hex);
        let class = name.split('/').next().unwrap_or("");

        if row.format != "auto" {
            let outcome = match row.format.as_str() {
                "pssz8" => explicit::<Pssz8>(&schema, &root, &bytes, name),
                "pssz16" => explicit::<Pssz16>(&schema, &root, &bytes, name),
                "pssz32" => explicit::<Pssz32>(&schema, &root, &bytes, name),
                format => panic!("{name}: unknown format {format}"),
            };
            explicit_width += 1;
            explicit_nested_caps += usize::from(class == "cap-overflow-explicit-nested");
            if name.starts_with("width/cap/") && row.expect == "reject" {
                explicit_top_caps += 1;
            }
            if row.expect == "reject" {
                assert!(outcome.is_err(), "{name}: accepted");
            } else {
                assert_eq!(outcome, Ok(row.expect == "unknown"), "{name}");
            }
            continue;
        }

        let outcome = dynamic::validate_pssz(&schema, &root, &bytes);
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

        if class == "string-raw-non-utf8" {
            // The IR tells a raw string (Custom "string") from a UTF-8 one
            // (Custom "utf8"): these frames are valid through the schema,
            // as for the typed raw string (checked above), and have no
            // JSON or I-JSON text.
            assert_eq!(row.expect, "canonical", "{name}");
            for text in [
                to_text::<Json>(&schema, &row.ty, &bytes),
                to_text::<IJson>(&schema, &row.ty, &bytes),
            ] {
                assert_eq!(text, Err("json: string is not valid UTF-8".to_string()), "{name}");
            }
            raw_non_utf8 += 1;
            continue;
        }

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
    // Every explicit-format vector was replayed, the capped types included.
    assert_eq!(explicit_width, fx.vectors.iter().filter(|row| row.format != "auto").count());
    assert_eq!(explicit_nested_caps, 15, "cap-overflow-explicit-nested");
    assert_eq!(explicit_top_caps, 12, "explicit top-level cap rejections");
    assert!(fx.types.iter().all(|t| t.ir), "a type has no IR");
}

#[test]
fn the_fixture_auto_widths_follow_from_the_ir() {
    // Both fixtures record the typed auto_pssz_width_v<T> of their types;
    // the IR alone must give the same width for EVERY one of them: records
    // (capped or not), boxes and what holds them, lists, arrays, tuples,
    // variants, options, strings and scalars.
    let fx: PsszFixture = load("pssz-vectors.json");
    let schema = schema_of(&fx.schema_ir);
    let mut checked = 0usize;
    let mut boxed = 0usize;
    for info in &fx.types {
        assert!(info.ir, "{}: no IR", info.id);
        let root = AnyType::named(info.id.as_str());
        let width = dynamic::auto_width(&schema, &root)
            .unwrap_or_else(|e| panic!("{}: auto_width: {e:?}", info.id));
        assert_eq!(Some(width), info.auto_width, "{}", info.id);
        checked += 1;
        boxed += usize::from(info.id.starts_with("Box") || info.id.contains("box<"));
    }
    println!("pssz fixture: auto widths checked for {checked} types");
    assert!(checked >= 220, "checked {checked}");
    assert!(boxed >= 12, "boxed {boxed}");
    // A box gives its container no inferred bound; a cap restores one.
    let width = |id: &str| dynamic::auto_width(&schema, &AnyType::named(id)).unwrap();
    assert_eq!(
        [width("BDirect"), width("BoxRec"), width("BoxCapRec"), width("BoxOptRec")],
        [1, 4, 1, 4]
    );
    assert_eq!([width("BoxVecRec"), width("BoxVecCapRec"), width("box<BLeaf>")], [4, 2, 4]);

    let fx: JsonFixture = load("json-vectors.json");
    let schema = schema_of(&fx.schema_ir);
    let mut checked = 0usize;
    for info in &fx.types {
        assert_eq!(info.auto_width.is_some(), info.pssz == Some(true), "{}", info.id);
        let Some(recorded) = info.auto_width else { continue };
        let root = AnyType::named(info.id.as_str());
        let width = dynamic::auto_width(&schema, &root)
            .unwrap_or_else(|e| panic!("{}: auto_width: {e:?}", info.id));
        assert_eq!(width, recorded, "{}", info.id);
        checked += 1;
    }
    println!("json fixture: auto widths checked for {checked} types");
    assert!(checked >= 49, "checked {checked}");
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

/// Malformed schemas: all three languages refuse the same ones, with an
/// error, at every schema-driven entry point; their well-formed neighbours
/// work. (C++ schema_conformance_tests.cpp, JS pssz-conformance.test.ts.)
#[test]
fn malformed_schemas_are_refused_and_their_neighbours_accepted() {
    let fx: JsonFixture = load("json-vectors.json");
    assert!(fx.schema_rejects.len() >= 25, "{}", fx.schema_rejects.len());
    assert!(fx.schema_accepts.len() >= 8, "{}", fx.schema_accepts.len());
    let mut names = BTreeSet::new();
    for row in &fx.schema_rejects {
        let name = &row.name;
        assert!(names.insert(name.clone()), "{name}: duplicate");
        // The IR itself is a valid `Schema` value: it is its meaning that is refused.
        let schema = schema_of(&row.schema_ir);
        let root = AnyType::named(row.root.clone());
        assert!(dynamic::auto_width(&schema, &root).is_err(), "{name}: auto_width");
        for bytes in [&[][..], &[0u8][..], &[0u8; 4][..], &[0u8; 8][..]] {
            assert!(dynamic::validate_pssz(&schema, &root, bytes).is_err(), "{name}: validate");
            assert!(dynamic::decode_exact::<Pssz32>(&schema, &root, bytes).is_err(), "{name}: pssz32");
            assert!(dynamic::pssz_to_json::<Json>(&schema, &root, bytes).is_err(), "{name}: to json");
        }
        for text in ["0", "\"\"", "null", "[]", "{}", "false"] {
            assert!(dynamic::json_to_pssz::<Json>(&schema, &root, text.as_bytes()).is_err(), "{name}: {text}");
            assert!(dynamic::json_to_pssz::<IJson>(&schema, &root, text.as_bytes()).is_err(), "{name}: {text}");
            assert!(dynamic::json_to_value::<IJson>(&schema, &root, text.as_bytes()).is_err(), "{name}: {text}");
        }
        assert!(
            psio::pssz::upgrade::validate_optional_upgrade(&schema, &root, &schema, &root).is_err(),
            "{name}: upgrade"
        );
    }
    for row in &fx.schema_accepts {
        let name = &row.name;
        assert!(names.insert(name.clone()), "{name}: duplicate");
        let schema = schema_of(&row.schema_ir);
        let root = AnyType::named(row.root.clone());
        let bytes = from_hex(&row.pssz);
        assert_eq!(dynamic::validate_pssz(&schema, &root, &bytes), Ok(false), "{name}");
        assert_eq!(dynamic::pssz_to_json::<Json>(&schema, &root, &bytes).unwrap().0, row.json, "{name}");
        assert_eq!(dynamic::pssz_to_json::<IJson>(&schema, &root, &bytes).unwrap().0, row.ijson, "{name}");
        assert_eq!(dynamic::json_to_pssz::<Json>(&schema, &root, row.json.as_bytes()).unwrap(), bytes, "{name}");
        assert_eq!(dynamic::json_to_pssz::<IJson>(&schema, &root, row.ijson.as_bytes()).unwrap(), bytes, "{name}");
        assert!(dynamic::auto_width(&schema, &root).is_ok(), "{name}");
    }
}

/// The IR root says which version of the IR it is written in; a schema
/// without the marker still decodes (the field was appended) and is refused
/// by the codecs with an error that says why.
#[test]
fn the_ir_version_marker_is_required_and_backward_compatible() {
    use psio::pssz::schema_ir::IR_VERSION;
    let fx: JsonFixture = load("json-vectors.json");
    let current = schema_of(&fx.schema_ir);
    assert_eq!(current.ir_version, Some(IR_VERSION));
    assert_eq!(Schema::default().ir_version, Some(IR_VERSION));

    // The same schema as it was written before the marker existed: the
    // bytes are a prefix-compatible encoding, and decode with no version.
    let mut old = current.clone();
    old.ir_version = None;
    let old_bytes = psio::pssz::try_to_pssz::<Pssz, Schema>(&old).unwrap();
    let new_bytes = from_hex(&fx.schema_ir);
    assert_ne!(old_bytes, new_bytes);
    pssz_validate::<Pssz, Schema>(&old_bytes).expect("an unversioned schema is still a Schema");
    let decoded = from_pssz::<Pssz, Schema>(&old_bytes).unwrap();
    assert_eq!(decoded.ir_version, None);
    assert_eq!(decoded.types, current.types);
    // JSON without the member reads the same way.
    let text = serde_json::to_string(&old).unwrap().replace(",\"ir_version\":null", "");
    assert!(!text.contains("ir_version"));
    assert_eq!(serde_json::from_str::<Schema>(&text).unwrap().ir_version, None);

    let some_type = fx.types.iter().find(|t| t.ir && t.pssz == Some(true)).expect("a type");
    let root = AnyType::named(some_type.id.clone());
    assert!(dynamic::auto_width(&current, &root).is_ok());
    for (schema, what) in [
        (decoded, "no ir_version marker"),
        (Schema { ir_version: Some(IR_VERSION + 1), ..current.clone() }, "newer than this library"),
        (Schema { ir_version: Some(0), ..current.clone() }, "newer than this library"),
    ] {
        let failures = [
            dynamic::auto_width(&schema, &root).err(),
            dynamic::validate(&schema, &root, &[]).err(),
            dynamic::encode::<Pssz>(&schema, &root, &dynamic::Value::Bool(true)).err(),
            dynamic::decode_exact::<Pssz8>(&schema, &root, &[]).err(),
            dynamic::layout::<Pssz>(&schema, &root).err(),
            psio::pssz::upgrade::validate_optional_upgrade(&schema, &root, &schema, &root).err(),
        ];
        for failure in failures {
            let failure = failure.expect("refused");
            assert!(failure.0.contains(what), "{} / {what}", failure.0);
        }
        let text = dynamic::json_to_pssz::<Json>(&schema, &root, b"0").unwrap_err();
        assert!(text.to_string().contains(what), "{text}");
    }
}
