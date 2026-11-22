//! Cross-language JSON / I-JSON conformance: consumes
//! conformance/json-vectors.json, the fixture emitted from the C++
//! implementation (cpp/tests/json_conformance_emit.cpp) and also checked by
//! ctest (cpp/tests/json_conformance_tests.cpp).
//!
//! * `value` rows carry the `json` and `ijson` texts of one value: Rust must
//!   read both to the same value and write both back byte for byte.
//! * `accept` rows are non-canonical text Rust must read and re-write as
//!   `canonical`.
//! * `reject` rows must be rejected.

use psio::json::{self, IJson, Json, JsonFormat, JsonText, TextForm};
use psio::pssz::RawString;
use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Debug;

#[derive(Debug, Clone, PartialEq, psio::JsonText)]
struct Inner {
    id: u64,
    label: String,
    flag: bool,
}

#[derive(Debug, Clone, PartialEq, psio::JsonText)]
struct Outer {
    inner: Inner,
    items: Vec<Inner>,
    maybe: Option<Inner>,
    blob: Vec<u8>,
    note: Option<RawString>,
    count: u32,
    ratio: f64,
    pair: (u32, i64),
}

// Member-level integer form overrides, in each direction.
#[derive(Debug, Clone, PartialEq, psio::JsonText)]
struct Overrides {
    #[json(json_string)]
    a: u64,
    #[json(ijson_number)]
    b: u64,
    #[json(json_string, ijson_string)]
    c: u32,
    #[json(ijson_number)]
    d: Vec<u64>,
    #[json(json_string)]
    e: Option<i64>,
    f: i64,
}

// Type-level override: applies to every integer member.
#[derive(Debug, Clone, PartialEq, psio::JsonText)]
#[json(json_string, ijson_number)]
struct TypeLevel {
    x: u64,
    y: i64,
    z: u32,
}

#[derive(Debug, Clone, PartialEq, psio::JsonText)]
struct AllOptional {
    a: Option<u32>,
    b: Option<String>,
}

// The C++ type is std::variant<uint32_t, uint64_t, std::string, Inner,
// std::vector<uint8_t>>; its I-JSON names are the alternatives' type names.
#[derive(Debug, Clone, PartialEq, psio::JsonText)]
enum Choice {
    #[json(name = "uint32")]
    U32(u32),
    #[json(name = "uint64")]
    U64(u64),
    #[json(name = "string")]
    Text(RawString),
    Inner(Inner),
    #[json(name = "uint8[]")]
    Bytes(Vec<u8>),
}

#[derive(Debug, Clone, PartialEq, psio::JsonText)]
struct Holder {
    choice: Choice,
    choices: Vec<Choice>,
}

// A custom text form: "major.minor".
#[derive(Debug, Clone, PartialEq)]
struct Version {
    major: u16,
    minor: u16,
}
impl TextForm for Version {
    fn to_text(&self, out: &mut String) {
        out.push_str(&format!("{}.{}", self.major, self.minor));
    }
    fn from_text(text: &str) -> Option<Self> {
        let part = |s: &str| -> Option<u16> {
            let canonical = !s.is_empty()
                && s.bytes().all(|b| b.is_ascii_digit())
                && (s == "0" || !s.starts_with('0'));
            if canonical { s.parse().ok() } else { None }
        };
        let (major, minor) = text.split_once('.')?;
        Some(Version { major: part(major)?, minor: part(minor)? })
    }
}
psio::json_text_form!(Version);

#[derive(Debug, Clone, PartialEq, psio::JsonText)]
struct Release {
    version: Version,
    name: String,
    history: Vec<Version>,
}

#[derive(Debug, Clone, PartialEq, psio::JsonText)]
struct Tree {
    children: Vec<Tree>,
}

#[derive(Debug, Clone, serde::Deserialize)]
struct Row {
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
}

#[derive(Debug, serde::Deserialize)]
struct Fixture {
    version: u32,
    vectors: Vec<Row>,
}

fn fixture() -> Fixture {
    let path = format!("{}/../../conformance/json-vectors.json", env!("CARGO_MANIFEST_DIR"));
    let text = std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{path}: {e}"));
    serde_json::from_str(&text).expect("fixture JSON")
}

fn input(row: &Row) -> Vec<u8> {
    match (&row.text, &row.text_hex) {
        (Some(text), _) => text.as_bytes().to_vec(),
        (None, Some(hex)) => (0..hex.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap())
            .collect(),
        _ => panic!("{}: no text", row.name),
    }
}

/// A fixture type under format `F`, reachable through `psio::json` and
/// through the crate-root `psio::encode` / `decode` / `validate`.
trait Val<F: JsonFormat>:
    JsonText + PartialEq + Debug + psio::Encode<F> + psio::Decode<F> + psio::Validate<F>
{
}
impl<F: JsonFormat, T> Val<F> for T where
    T: JsonText + PartialEq + Debug + psio::Encode<F> + psio::Decode<F> + psio::Validate<F>
{
}

fn reads<F: JsonFormat, T: Val<F>>(text: &[u8]) -> Result<T, String> {
    json::validate::<F, T>(text).map_err(|e| format!("validate: {e}"))?;
    let value = json::from_slice::<F, T>(text).map_err(|e| format!("from_slice: {e}"))?;
    let root = psio::decode::<F, T>(text).map_err(|e| format!("psio::decode: {e}"))?;
    if root != value {
        return Err("psio::decode differs from from_slice".into());
    }
    psio::validate::<F, T, _>(text, &psio::DefaultSafe).map_err(|e| format!("psio::validate: {e}"))?;
    Ok(value)
}

fn writes<F: JsonFormat, T: Val<F>>(value: &T, want: &str) -> Result<(), String> {
    let got = json::to_string::<F, T>(value).map_err(|e| format!("to_string: {e}"))?;
    if got != want {
        return Err(format!("writes {got}, want {want}"));
    }
    if psio::encode::<F, T>(value).map_err(|e| e.to_string())? != want.as_bytes() {
        return Err("psio::encode differs".into());
    }
    if json::to_vec::<F, T>(value).map_err(|e| e.to_string())? != want.as_bytes() {
        return Err("encode is not deterministic".into());
    }
    Ok(())
}

fn rejects<F: JsonFormat, T: Val<F>>(text: &[u8]) -> Result<(), String> {
    if json::validate::<F, T>(text).is_ok() {
        return Err("validate accepts".into());
    }
    if let Ok(v) = json::from_slice::<F, T>(text) {
        return Err(format!("from_slice accepts as {v:?}"));
    }
    if psio::decode::<F, T>(text).is_ok() {
        return Err("psio::decode accepts".into());
    }
    Ok(())
}

fn check_row<T: Val<Json> + Val<IJson>>(row: &Row) -> Result<(), String> {
    fn accept<F: JsonFormat, T: Val<F>>(row: &Row) -> Result<(), String> {
        let canonical = row.canonical.as_ref().ok_or("accept row needs canonical")?;
        let a: T = reads::<F, T>(&input(row))?;
        writes::<F, T>(&a, canonical)?;
        let b: T = reads::<F, T>(canonical.as_bytes()).map_err(|e| format!("canonical {e}"))?;
        if a != b {
            return Err("canonical text is a different value".into());
        }
        Ok(())
    }
    match (row.kind.as_str(), row.format.as_deref()) {
        ("value", None) => {
            let json_text = row.json.as_ref().ok_or("value row needs json")?;
            let ijson_text = row.ijson.as_ref().ok_or("value row needs ijson")?;
            let a: T = reads::<Json, T>(json_text.as_bytes()).map_err(|e| format!("json {e}"))?;
            let b: T = reads::<IJson, T>(ijson_text.as_bytes()).map_err(|e| format!("ijson {e}"))?;
            if a != b {
                return Err(format!("json and ijson texts are different values: {a:?} / {b:?}"));
            }
            writes::<Json, T>(&a, json_text).map_err(|e| format!("json {e}"))?;
            writes::<IJson, T>(&a, ijson_text).map_err(|e| format!("ijson {e}"))
        }
        ("accept", Some("json")) => accept::<Json, T>(row),
        ("accept", Some("ijson")) => accept::<IJson, T>(row),
        ("reject", Some("json")) => rejects::<Json, T>(&input(row)),
        ("reject", Some("ijson")) => rejects::<IJson, T>(&input(row)),
        (kind, format) => Err(format!("unknown kind/format {kind} {format:?}")),
    }
}

type Check = fn(&Row) -> Result<(), String>;

fn registry() -> BTreeMap<&'static str, Check> {
    let mut map: BTreeMap<&'static str, Check> = BTreeMap::new();
    macro_rules! types {
        ($($id:literal => $ty:ty,)*) => {$( map.insert($id, check_row::<$ty>); )*};
    }
    types! {
        "bool" => bool,
        "u8" => u8, "i8" => i8, "u16" => u16, "i16" => i16,
        "u32" => u32, "i32" => i32, "u64" => u64, "i64" => i64,
        "f32" => f32, "f64" => f64,
        "string" => String,
        "raw_string" => RawString,
        "bytes" => Vec<u8>,
        "bytes4" => [u8; 4],
        "list<u64>" => Vec<u64>,
        "list<u32>" => Vec<u32>,
        "list<bytes>" => Vec<Vec<u8>>,
        "list<optional<i64>>" => Vec<Option<i64>>,
        "array<u16,3>" => [u16; 3],
        "optional<u64>" => Option<u64>,
        "optional<string>" => Option<String>,
        "optional<bytes>" => Option<Vec<u8>>,
        "tuple<>" => (),
        "tuple<u32,string,u64>" => (u32, String, u64),
        "tuple<tuple<u8,i64>,tuple<>,list<bytes>>" => ((u8, i64), (), Vec<Vec<u8>>),
        "Inner" => Inner,
        "Outer" => Outer,
        "Overrides" => Overrides,
        "TypeLevel" => TypeLevel,
        "AllOptional" => AllOptional,
        "Choice" => Choice,
        "Holder" => Holder,
        "Version" => Version,
        "Release" => Release,
        "Tree" => Tree,
    }
    map
}

#[test]
fn every_vector_holds_on_every_entry_point() {
    let fixture = fixture();
    assert_eq!(fixture.version, 1);
    let registry = registry();
    let mut failures = vec![];
    for row in &fixture.vectors {
        let Some(check) = registry.get(row.ty.as_str()) else {
            failures.push(format!("{}: unmapped type id {}", row.name, row.ty));
            continue;
        };
        if let Err(e) = check(row) {
            failures.push(format!("{}: {e}", row.name));
        }
    }
    let shown: Vec<_> = failures.iter().take(40).cloned().collect();
    assert!(
        failures.is_empty(),
        "{} of {} vectors failed:\n{}",
        failures.len(),
        fixture.vectors.len(),
        shown.join("\n")
    );
    assert!(fixture.vectors.len() >= 900, "fixture shrank");
}

#[test]
fn the_fixture_covers_every_type_kind_and_format() {
    let fixture = fixture();
    let mut names = BTreeSet::new();
    let mut valued = BTreeSet::new();
    let mut count: BTreeMap<String, usize> = BTreeMap::new();
    for row in &fixture.vectors {
        assert!(names.insert(row.name.clone()), "duplicate row {}", row.name);
        if row.kind == "value" {
            valued.insert(row.ty.clone());
        }
        *count
            .entry(format!("{}:{}", row.kind, row.format.as_deref().unwrap_or("both")))
            .or_default() += 1;
    }
    for id in registry().keys() {
        assert!(valued.contains(*id), "no value vector for {id}");
    }
    assert!(count["value:both"] >= 100);
    assert!(count["accept:json"] >= 10 && count["accept:ijson"] >= 10);
    assert!(count["reject:json"] >= 100 && count["reject:ijson"] >= 150);
}

#[test]
fn native_values_have_the_fixture_texts() {
    // The same values the C++ emitter builds, written from Rust values.
    let text = |v: &dyn Fn() -> Result<String, json::JsonTextError>| v().unwrap();
    assert_eq!(text(&|| json::to_string::<IJson, _>(&u64::MAX)), "\"18446744073709551615\"");
    assert_eq!(text(&|| json::to_string::<Json, _>(&u64::MAX)), "18446744073709551615");
    assert_eq!(text(&|| json::to_string::<IJson, _>(&i64::MIN)), "\"-9223372036854775808\"");
    assert_eq!(text(&|| json::to_string::<IJson, _>(&(1u64 << 53))), "\"9007199254740992\"");
    assert_eq!(text(&|| json::to_string::<IJson, _>(&((1u64 << 53) - 1))), "9007199254740991");
    assert_eq!(text(&|| json::to_string::<IJson, _>(&((1i64 << 53) - 1))), "9007199254740991");
    assert_eq!(text(&|| json::to_string::<IJson, _>(&-((1i64 << 53) - 1))), "-9007199254740991");
    assert_eq!(text(&|| json::to_string::<IJson, _>(&-(1i64 << 53))), "\"-9007199254740992\"");
    assert_eq!(text(&|| json::to_string::<IJson, _>(&0u64)), "0");
    assert_eq!(text(&|| json::to_string::<IJson, _>(&i64::MAX)), "\"9223372036854775807\"");
    assert_eq!(text(&|| json::to_string::<IJson, _>(&((1u64 << 53) + 1))), "\"9007199254740993\"");
    assert_eq!(text(&|| json::to_string::<IJson, _>(&u32::MAX)), "4294967295");
    assert_eq!(text(&|| json::to_string::<IJson, _>(&vec![0x01u8, 0xff])), "\"01FF\"");
    assert_eq!(text(&|| json::to_string::<Json, _>(&vec![0x01u8, 0xff])), "[1,255]");
    assert_eq!(text(&|| json::to_string::<IJson, _>(&Vec::<u8>::new())), "\"\"");
    assert_eq!(text(&|| json::to_string::<IJson, _>(&[0xdeu8, 0xad, 0xbe, 0xef])), "\"DEADBEEF\"");
    assert_eq!(text(&|| json::to_string::<IJson, _>(&())), "[]");
    assert_eq!(text(&|| json::to_string::<Json, _>(&())), "[]");
    assert_eq!(
        text(&|| json::to_string::<IJson, _>(&(7u32, "t".to_string(), 1u64 << 60))),
        "[7,\"t\",\"1152921504606846976\"]"
    );
    let inner = Inner { id: 7, label: "seven".into(), flag: true };
    assert_eq!(
        text(&|| json::to_string::<IJson, _>(&Choice::Inner(inner.clone()))),
        r#"{"Inner":{"id":7,"label":"seven","flag":true}}"#
    );
    assert_eq!(
        text(&|| json::to_string::<Json, _>(&Choice::Inner(inner.clone()))),
        r#"[3,{"id":7,"label":"seven","flag":true}]"#
    );
    assert_eq!(text(&|| json::to_string::<IJson, _>(&Choice::U32(42))), r#"{"uint32":42}"#);
    assert_eq!(text(&|| json::to_string::<IJson, _>(&Choice::U64(42))), r#"{"uint64":42}"#);
    let overrides = Overrides { a: 1, b: 2, c: 3, d: vec![4, 5], e: Some(-6), f: -7 };
    assert_eq!(
        text(&|| json::to_string::<Json, _>(&overrides)),
        r#"{"a":"1","b":2,"c":"3","d":[4,5],"e":"-6","f":-7}"#
    );
    assert_eq!(
        text(&|| json::to_string::<IJson, _>(&overrides)),
        r#"{"a":1,"b":2,"c":"3","d":[4,5],"e":-6,"f":-7}"#
    );
    assert_eq!(
        text(&|| json::to_string::<IJson, _>(&Version { major: 12, minor: 345 })),
        "\"12.345\""
    );
}
