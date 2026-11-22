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
//! * rows with `pssz` carry the typed C++ `psio::pssz` bytes of the value:
//!   the typed Rust `Pssz` codec must produce and read the same bytes.
//!
//! The types also derive `ToPsszSchema`: the IR Rust generates for them must
//! drive the schema path (`psio::pssz::dynamic`) to the bytes and texts of
//! the typed Rust codecs. (The IR C++ recorded in the fixture is exercised by
//! schema_conformance.rs.)

use psio::json::{self, IJson, Json, JsonFormat, JsonText, TextForm};
use psio::pssz::schema_ir::{schema_for, ToPsszSchema};
use psio::pssz::{
    dynamic, BoundedList, BoundedRawString, BoundedString, Pssz, PsszPack, PsszUnpack, RawString,
    Uint256,
};
use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Debug;

#[derive(
    Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema,
)]
struct Inner {
    id: u64,
    label: String,
    flag: bool,
}

#[derive(
    Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema,
)]
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
#[derive(
    Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema,
)]
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
#[derive(
    Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema,
)]
#[json(json_string, ijson_number)]
struct TypeLevel {
    x: u64,
    y: i64,
    z: u32,
}

#[derive(
    Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema,
)]
struct AllOptional {
    a: Option<u32>,
    b: Option<String>,
}

// The C++ type is std::variant<uint32_t, uint64_t, std::string, Inner,
// std::vector<uint8_t>>; its I-JSON names are the alternatives' type names.
#[derive(
    Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema,
)]
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

#[derive(
    Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema,
)]
struct Holder {
    choice: Choice,
    choices: Vec<Choice>,
}

// A custom text form: "major.minor". On the wire it is the record of its two
// members; the Schema IR records Custom { that record, "Version" }.
#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema)]
#[pssz(custom)]
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

#[derive(
    Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema,
)]
struct Release {
    version: Version,
    name: String,
    history: Vec<Version>,
}

// A custom text form over a 64-bit integer: an account name of up to 12
// characters of [a-z1-5.], 5 bits each, most significant first. On the wire
// it is its u64; the Schema IR records Custom { Int{64}, "AccountName" } (the
// id C++, Rust and JS register the handler under, not the type's name).
#[derive(Debug, Clone, Copy, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema)]
#[pssz(transparent, custom = "AccountName")]
struct Account(u64);
const ACCOUNT_ALPHABET: &[u8; 32] = b".12345abcdefghijklmnopqrstuvwxyz";
impl TextForm for Account {
    fn to_text(&self, out: &mut String) {
        let mut text: Vec<u8> =
            (0..12).map(|i| ACCOUNT_ALPHABET[((self.0 >> (5 * (11 - i))) & 31) as usize]).collect();
        while text.last() == Some(&b'.') {
            text.pop();
        }
        out.push_str(std::str::from_utf8(&text).expect("ASCII"));
    }
    fn from_text(text: &str) -> Option<Self> {
        // No trailing dot: one text per value.
        if text.len() > 12 || text.ends_with('.') {
            return None;
        }
        let mut value = 0u64;
        for i in 0..12 {
            let digit = match text.as_bytes().get(i) {
                Some(c) => ACCOUNT_ALPHABET.iter().position(|a| a == c)? as u64,
                None => 0,
            };
            value = (value << 5) | digit;
        }
        Some(Account(value))
    }
}
psio::json_text_form!(Account);

#[derive(
    Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema,
)]
struct Transfer {
    from: Account,
    to: Account,
    amount: u64,
    payer: Option<Account>,
    notify: Vec<Account>,
    since: Option<Version>,
}

/// The handlers of the fixture's custom text forms, from the types' own
/// `TextForm`: one call each, under the id their schema carries.
fn typed_handlers() -> dynamic::CustomHandlers {
    let mut handlers = dynamic::CustomHandlers::new();
    handlers.add_type::<Version>().unwrap().add_type::<Account>().unwrap();
    handlers
}

#[derive(
    Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema,
)]
struct Tree {
    children: Vec<Tree>,
}

// ── Length bounds, fixed layout, caps, enums ────────────────────────────────

// C++: utf8_string<8>, and std::vector / std::string / std::optional members
// with a length_bound. The inferred bound selects 1-byte offsets.
#[derive(
    Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema,
)]
struct Bounds {
    name: BoundedString<8>,
    tags: BoundedList<u32, 3>,
    memo: BoundedRawString<4>,
    blob: BoundedList<u8, 2>,
    note: Option<BoundedRawString<3>>,
}

// Fixed layout: no header.
#[derive(
    Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema,
)]
#[pssz(definition_will_not_change)]
struct Fixed {
    x: i32,
    y: i32,
    on: bool,
}

// C++ `enum class Color : std::uint8_t { red = 0, green = 1, blue = 7 }` with
// PSIO_REFLECT_ENUM: a declared value is written as its name. A C++ enum
// holds every value of its underlying type and the fixture has rows for
// unnamed ones (2, 9, 255), so the mirror carries the catch-all.
#[derive(
    Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema,
)]
#[repr(u8)]
enum Color {
    #[json(name = "red")]
    Red = 0,
    #[json(name = "green")]
    Green = 1,
    #[json(name = "blue")]
    Blue = 7,
    #[pssz(other)]
    Unknown(u8),
}

// C++ `enum class Status : std::int32_t { pending = -1, active = 2, closed = 7 }`:
// signed and sparse.
#[derive(
    Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema,
)]
#[repr(i32)]
enum Status {
    #[json(name = "pending")]
    Pending = -1,
    #[json(name = "active")]
    Active = 2,
    #[json(name = "closed")]
    Closed = 7,
    #[pssz(other)]
    Unknown(i32),
}

// 128- and 256-bit integers: decimal in both text formats.
#[derive(
    Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema,
)]
struct Wide {
    amount: u128,
    delta: i128,
    hash: Uint256,
    maybe: Option<u128>,
    list: Vec<i128>,
    status: Status,
}

// Explicit maxDynamicData cap: 2-byte offsets whatever the fields hold.
#[derive(
    Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema,
)]
#[pssz(max_dynamic_data = 300)]
struct Capped {
    id: u16,
    memo: RawString,
    at: Option<Fixed>,
    path: Vec<Fixed>,
}

#[derive(
    Debug, Clone, PartialEq, psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema,
)]
struct Shapes {
    color: Color,
    corners: [Fixed; 2],
    scale: f32,
    delta: i16,
    label: Option<String>,
    small: Option<u8>,
    capped: Capped,
    bounds: Option<Bounds>,
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
    /// Typed C++ `psio::pssz` bytes of the row's value (lowercase hex).
    pssz: Option<String>,
    /// Value rows of a type holding a custom text form: the text a schema
    /// reader writes with no handler registered (the wire type's own text).
    plain_json: Option<String>,
    plain_ijson: Option<String>,
}

#[derive(Debug, Clone, serde::Deserialize)]
struct TypeRow {
    id: String,
    /// The Schema IR describes the type.
    ir: bool,
    /// Value and accept rows of the type carry `pssz`.
    pssz: bool,
    /// The typed `auto_pssz_width_v<T>` (present when `pssz`).
    auto_width: Option<usize>,
    /// The type holds a custom text form: its value rows carry
    /// `plain_json` / `plain_ijson`.
    #[serde(default)]
    custom: bool,
}

#[derive(Debug, serde::Deserialize)]
struct Fixture {
    version: u32,
    /// The C++ Schema IR of every `ir` type (see schema_conformance.rs).
    schema_ir: String,
    types: Vec<TypeRow>,
    vectors: Vec<Row>,
}

fn unhex(hex: &str) -> Vec<u8> {
    assert!(hex.len() % 2 == 0, "odd hex");
    (0..hex.len()).step_by(2).map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap()).collect()
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

/// The text checks of one row; the value it holds (`None` for a reject row).
fn check_row<T: Val<Json> + Val<IJson>>(row: &Row) -> Result<Option<T>, String> {
    fn accept<F: JsonFormat, T: Val<F>>(row: &Row) -> Result<Option<T>, String> {
        let canonical = row.canonical.as_ref().ok_or("accept row needs canonical")?;
        let a: T = reads::<F, T>(&input(row))?;
        writes::<F, T>(&a, canonical)?;
        let b: T = reads::<F, T>(canonical.as_bytes()).map_err(|e| format!("canonical {e}"))?;
        if a != b {
            return Err("canonical text is a different value".into());
        }
        Ok(Some(a))
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
            writes::<IJson, T>(&a, ijson_text).map_err(|e| format!("ijson {e}"))?;
            Ok(Some(a))
        }
        ("accept", Some("json")) => accept::<Json, T>(row),
        ("accept", Some("ijson")) => accept::<IJson, T>(row),
        ("reject", Some("json")) => rejects::<Json, T>(&input(row)).map(|_| None),
        ("reject", Some("ijson")) => rejects::<IJson, T>(&input(row)).map(|_| None),
        (kind, format) => Err(format!("unknown kind/format {kind} {format:?}")),
    }
}

/// A type without typed PSSZ bytes in the fixture: the text checks only.
fn check_text<T: Val<Json> + Val<IJson>>(row: &Row) -> Result<(), String> {
    if row.pssz.is_some() {
        return Err("row carries pssz bytes but its type is registered as text-only".into());
    }
    check_row::<T>(row).map(|_| ())
}

/// A fixture type the typed `Pssz` codec encodes.
trait Packed: Val<Json> + Val<IJson> + PsszPack<Pssz> + PsszUnpack<Pssz> {}
impl<T: Val<Json> + Val<IJson> + PsszPack<Pssz> + PsszUnpack<Pssz>> Packed for T {}

/// The text checks, and the typed `Pssz` bytes of the value against the
/// row's `pssz` (the bytes of the typed C++ codec).
fn check_pssz<T: Packed>(row: &Row) -> Result<(), String> {
    let value = check_row::<T>(row)?;
    match (&value, &row.pssz) {
        (None, None) => Ok(()),
        (Some(value), Some(hex)) => {
            let want = unhex(hex);
            let got = psio::pssz::try_to_pssz::<Pssz, T>(value).map_err(|e| format!("pssz {e:?}"))?;
            if got != want {
                return Err(format!("typed pssz gives {got:02x?}, want {hex}"));
            }
            let back =
                psio::pssz::from_pssz::<Pssz, T>(&want).map_err(|e| format!("from_pssz {e:?}"))?;
            if back != *value {
                return Err(format!("from_pssz gives {back:?}, want {value:?}"));
            }
            psio::pssz::from_pssz_exact::<Pssz, T>(&want)
                .map(|_| ())
                .map_err(|e| format!("from_pssz_exact {e:?}"))
        }
        (Some(_), None) => Err("value / accept row of a pssz type has no pssz bytes".into()),
        (None, Some(_)) => Err("reject row carries pssz bytes".into()),
    }
}

/// The IR Rust generates for `T` must drive the schema path to the results
/// of the typed Rust codecs: same bytes, same texts, same accept / reject.
fn check_rust_ir<T: Val<Json> + Val<IJson> + ToPsszSchema>(
    row: &Row,
    typed_pssz: &dyn Fn(&T) -> Option<Vec<u8>>,
) -> Result<(), String> {
    // The handlers are the types' own text forms; a type without a custom
    // form never looks one up.
    let handlers = typed_handlers();
    let handlers = &handlers;
    // Shadow the plain entry points: every call below goes through the
    // registry.
    mod dynamic {
        pub use psio::pssz::dynamic::{auto_width, validate_pssz};
        pub use psio::pssz::dynamic::{
            json_to_pssz_with as json_to_pssz, pssz_to_json_with as pssz_to_json,
        };
    }
    let (schema, root) = schema_for::<T>().map_err(|e| format!("schema_for {e:?}"))?;
    // One text read through the schema: the bytes it gives, which must turn
    // back into the typed texts of the typed value.
    let convert = |ijson: bool, text: &[u8]| -> Result<Vec<u8>, String> {
        let (bytes, value): (Vec<u8>, T) = if ijson {
            (
                dynamic::json_to_pssz::<IJson>(&schema, &root, text, handlers).map_err(|e| e.to_string())?,
                json::from_slice::<IJson, T>(text).map_err(|e| e.to_string())?,
            )
        } else {
            (
                dynamic::json_to_pssz::<Json>(&schema, &root, text, handlers).map_err(|e| e.to_string())?,
                json::from_slice::<Json, T>(text).map_err(|e| e.to_string())?,
            )
        };
        if let Some(want) = typed_pssz(&value) {
            if bytes != want {
                return Err(format!("schema path gives {bytes:02x?}, typed pssz {want:02x?}"));
            }
        }
        if dynamic::validate_pssz(&schema, &root, &bytes).map_err(|e| e.to_string())? {
            return Err("schema path reports unknown fields in its own bytes".into());
        }
        // Same text, or the same refusal (a string holding a Unicode
        // noncharacter is JSON but has no I-JSON text).
        let json_text = dynamic::pssz_to_json::<Json>(&schema, &root, &bytes, handlers)
            .map(|(text, unknown)| {
                assert!(!unknown);
                text
            })
            .map_err(|e| e.what().to_string());
        let typed = json::to_string::<Json, T>(&value).map_err(|e| e.what.to_string());
        if json_text != typed {
            return Err(format!("schema path writes json {json_text:?}, typed {typed:?}"));
        }
        let ijson_text = dynamic::pssz_to_json::<IJson>(&schema, &root, &bytes, handlers)
            .map(|(text, unknown)| {
                assert!(!unknown);
                text
            })
            .map_err(|e| e.what().to_string());
        let typed = json::to_string::<IJson, T>(&value).map_err(|e| e.what.to_string());
        if ijson_text != typed {
            return Err(format!("schema path writes ijson {ijson_text:?}, typed {typed:?}"));
        }
        Ok(bytes)
    };
    match (row.kind.as_str(), row.format.as_deref()) {
        ("value", None) => {
            let a = convert(false, row.json.as_ref().ok_or("no json")?.as_bytes())?;
            let b = convert(true, row.ijson.as_ref().ok_or("no ijson")?.as_bytes())?;
            if a != b {
                return Err("json and ijson texts give different bytes".into());
            }
            Ok(())
        }
        ("accept", Some(format)) => convert(format == "ijson", &input(row)).map(|_| ()),
        ("reject", Some("json")) => match dynamic::json_to_pssz::<Json>(&schema, &root, &input(row), handlers) {
            Ok(_) => Err("schema path accepts".into()),
            Err(_) => Ok(()),
        },
        ("reject", Some("ijson")) => {
            match dynamic::json_to_pssz::<IJson>(&schema, &root, &input(row), handlers) {
                Ok(_) => Err("schema path accepts".into()),
                Err(_) => Ok(()),
            }
        }
        (kind, format) => Err(format!("unknown kind/format {kind} {format:?}")),
    }
}

type Check = fn(&Row) -> Result<(), String>;

/// How a fixture type id is checked.
#[derive(Clone, Copy)]
struct Entry {
    /// Typed text checks, plus the typed `Pssz` bytes when `pssz`.
    check: Check,
    /// The fixture carries typed pssz bytes for the type.
    pssz: bool,
    /// Schema path over the Rust-generated IR (`None`: no `ToPsszSchema`).
    rust_ir: Option<Check>,
    /// `dynamic::auto_width` of the Rust IR against `auto_pssz_width::<T>()`.
    width: Option<fn() -> Result<(), String>>,
    /// `auto_pssz_width::<T>()`, for the fixture's recorded `auto_width`.
    typed_width: Option<fn() -> usize>,
}

fn rust_ir_with_pssz<T: Packed + ToPsszSchema>(row: &Row) -> Result<(), String> {
    check_rust_ir::<T>(row, &|value| psio::pssz::try_to_pssz::<Pssz, T>(value).ok())
}
fn rust_ir_text_only<T: Packed + ToPsszSchema>(row: &Row) -> Result<(), String> {
    // The fixture has no bytes for the type, but Rust has a typed codec.
    rust_ir_with_pssz::<T>(row)
}
fn width_agrees<T: PsszPack<Pssz> + ToPsszSchema>() -> Result<(), String> {
    let (schema, root) = schema_for::<T>().map_err(|e| format!("schema_for {e:?}"))?;
    let got = dynamic::auto_width(&schema, &root).map_err(|e| format!("auto_width {e:?}"))?;
    let want = psio::pssz::auto_pssz_width::<T>();
    if got != want {
        return Err(format!("auto_width from the Rust IR is {got}, auto_pssz_width {want}"));
    }
    Ok(())
}

fn registry() -> BTreeMap<&'static str, Entry> {
    let mut map: BTreeMap<&'static str, Entry> = BTreeMap::new();
    // `types!`: typed pssz bytes in the fixture and a Rust-generated IR.
    // `bounded!`: no pssz bytes in the fixture (C++ `psio::bounded` has no
    // pssz encoding); Rust still has a typed codec and an IR for them.
    macro_rules! types {
        ($($id:literal => $ty:ty,)*) => {$( map.insert($id, Entry {
            check: check_pssz::<$ty>,
            pssz: true,
            rust_ir: Some(rust_ir_with_pssz::<$ty>),
            width: Some(width_agrees::<$ty>),
            typed_width: Some(psio::pssz::auto_pssz_width::<$ty>),
        }); )*};
    }
    macro_rules! bounded {
        ($($id:literal => $ty:ty,)*) => {$( map.insert($id, Entry {
            check: check_text::<$ty>,
            pssz: false,
            rust_ir: Some(rust_ir_text_only::<$ty>),
            width: Some(width_agrees::<$ty>),
            typed_width: None,
        }); )*};
    }
    bounded! {
        "list<u16><=3" => BoundedList<u16, 3>,
        "bytes<=2" => BoundedList<u8, 2>,
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
        "Tree" => Tree,
        "Bounds" => Bounds,
        "Fixed" => Fixed,
        "Color" => Color,
        "Status" => Status,
        "u128" => u128,
        "i128" => i128,
        "u256" => Uint256,
        "Wide" => Wide,
        "Capped" => Capped,
        "Shapes" => Shapes,
        // Custom text forms: Custom nodes in the IR, read and written
        // through the handler registry on the schema path.
        "Version" => Version,
        "Release" => Release,
        "Account" => Account,
        "Transfer" => Transfer,
        "string<=4" => BoundedString<4>,
    }
    // `BoundedList<u8, N>` is a byte payload: nothing on the wire has an
    // offset width. The typed codec nominally reports the list rule (4, a
    // bounded list keeps the Vec's width) while the IR root is the "hex"
    // adapter, for which `auto_width` (like C++ `auto_width`) reports the
    // width class of the byte bound (1). Neither is ever written.
    map.get_mut("bytes<=2").expect("registered").width = None;
    map
}

#[test]
fn every_vector_holds_on_every_entry_point() {
    let fixture = fixture();
    assert_eq!(fixture.version, 1);
    let registry = registry();
    let mut failures = vec![];
    for row in &fixture.vectors {
        let Some(entry) = registry.get(row.ty.as_str()) else {
            failures.push(format!("{}: unmapped type id {}", row.name, row.ty));
            continue;
        };
        if let Err(e) = (entry.check)(row) {
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
    let registry = registry();
    for id in registry.keys() {
        assert!(valued.contains(*id), "no value vector for {id}");
    }
    // The fixture's type table and the registry name the same ids, and agree
    // on which types carry pssz bytes and which the IR describes.
    let listed: BTreeSet<&str> = fixture.types.iter().map(|t| t.id.as_str()).collect();
    assert_eq!(listed.len(), fixture.types.len(), "duplicate type id");
    assert_eq!(listed, registry.keys().copied().collect::<BTreeSet<_>>());
    assert!(registry.len() >= 44);
    for ty in &fixture.types {
        let entry = &registry[ty.id.as_str()];
        // The typed width the fixture records is the typed Rust width.
        assert_eq!(ty.auto_width.is_some(), ty.pssz, "{}: auto_width", ty.id);
        assert_eq!(entry.typed_width.map(|width| width()), ty.auto_width, "{}: width", ty.id);
        assert_eq!(
            ty.custom,
            matches!(ty.id.as_str(), "Version" | "Release" | "Account" | "Transfer"),
            "{}: custom",
            ty.id
        );
        assert_eq!(entry.pssz, ty.pssz, "{}: pssz", ty.id);
        assert_eq!(entry.rust_ir.is_some(), ty.ir, "{}: ir", ty.id);
        assert!(valued.contains(&ty.id), "no value vector for {}", ty.id);
    }
    for row in &fixture.vectors {
        let ty = fixture.types.iter().find(|t| t.id == row.ty).expect("listed type");
        assert_eq!(row.pssz.is_some(), ty.pssz && row.kind != "reject", "{}: pssz", row.name);
        let plain = ty.custom && row.kind == "value";
        assert_eq!(row.plain_json.is_some(), plain, "{}: plain_json", row.name);
        assert_eq!(row.plain_ijson.is_some(), plain, "{}: plain_ijson", row.name);
    }
    assert!(!fixture.schema_ir.is_empty() && fixture.schema_ir.len() % 2 == 0);
    assert!(fixture.vectors.iter().filter(|r| r.pssz.is_some()).count() >= 600);
    assert!(count["value:both"] >= 100);
    assert!(count["accept:json"] >= 10 && count["accept:ijson"] >= 10);
    assert!(count["reject:json"] >= 100 && count["reject:ijson"] >= 150);
}

#[test]
fn the_rust_generated_ir_gives_the_typed_rust_results() {
    let fixture = fixture();
    let registry = registry();
    let mut failures = vec![];
    let (mut rows, mut types) = (0usize, 0usize);
    for (id, entry) in &registry {
        let Some(width) = entry.width else { continue };
        types += 1;
        if let Err(e) = width() {
            failures.push(format!("{id}: {e}"));
        }
    }
    for row in &fixture.vectors {
        let Some(check) = registry.get(row.ty.as_str()).and_then(|entry| entry.rust_ir) else {
            continue;
        };
        rows += 1;
        if let Err(e) = check(row) {
            failures.push(format!("{}: {e}", row.name));
        }
    }
    let shown: Vec<_> = failures.iter().take(40).cloned().collect();
    assert!(failures.is_empty(), "{} failed:\n{}", failures.len(), shown.join("\n"));
    assert!(types >= 41, "types with a Rust IR: {types}");
    assert!(rows >= 1000, "rows through the Rust IR: {rows}");
}

#[test]
fn the_enum_and_wide_integer_ir_rust_builds_is_the_ir_cpp_recorded() {
    use psio::pssz::schema_ir::{AnyType, AnyValue, Schema};
    let fixture = fixture();
    let bytes = unhex(&fixture.schema_ir);
    let recorded = psio::pssz::from_pssz::<Pssz, Schema>(&bytes).expect("schema_ir decodes");
    // The decoded IR re-encodes to the fixture's bytes: Enum included.
    assert_eq!(psio::pssz::try_to_pssz::<Pssz, _>(&recorded).unwrap(), bytes);
    fn same<T: ToPsszSchema>(recorded: &Schema, id: &str) -> AnyType {
        let (built, _) = schema_for::<T>().unwrap();
        // The fixture lists each type under its id, as an alias of the name
        // the C++ builder gave it.
        let rust = built.get(id).expect("built");
        let cpp = recorded.resolve(recorded.get(id).expect("recorded")).expect("resolves");
        assert_eq!(rust, cpp, "{id}");
        assert_eq!(
            psio::pssz::try_to_pssz::<Pssz, _>(rust).unwrap(),
            psio::pssz::try_to_pssz::<Pssz, _>(cpp).unwrap(),
            "{id}"
        );
        cpp.clone()
    }
    for ty in [same::<Color>(&recorded, "Color"), same::<Status>(&recorded, "Status")] {
        assert!(matches!(ty.value, AnyValue::Enum(_)));
    }
    // The catch-all is not an enumerator: three names each.
    let named = |id: &str| recorded.resolve(recorded.get(id).unwrap()).unwrap();
    let AnyValue::Enum(color) = &named("Color").value else { unreachable!() };
    let names: Vec<_> = color.members.iter().map(|m| (m.name.as_str(), m.value)).collect();
    assert_eq!(names, [("red", 0), ("green", 1), ("blue", 7)]);
    let AnyValue::Enum(status) = &named("Status").value else { unreachable!() };
    let names: Vec<_> = status.members.iter().map(|m| (m.name.as_str(), m.value)).collect();
    assert_eq!(names, [("pending", u64::MAX), ("active", 2), ("closed", 7)]);
    // The wide integers resolve to the same types.
    for (id, built) in [
        ("u128", schema_for::<u128>().unwrap()),
        ("i128", schema_for::<i128>().unwrap()),
        ("u256", schema_for::<Uint256>().unwrap()),
    ] {
        assert_eq!(
            built.0.resolve(&built.1).unwrap(),
            recorded.resolve(&AnyType::named(id)).unwrap(),
            "{id}"
        );
    }
}

#[test]
fn custom_text_forms_go_through_the_handler_registry_or_fall_back_to_the_wire_type() {
    use psio::pssz::schema_ir::{AnyType, AnyValue, Schema};
    let fixture = fixture();
    // The IR Rust builds for the custom types is the IR C++ recorded.
    let recorded = psio::pssz::from_pssz::<Pssz, Schema>(&unhex(&fixture.schema_ir)).unwrap();
    let named = |schema: &Schema, id: &str| -> AnyType {
        schema.resolve(schema.get(id).expect("listed")).expect("resolves").clone()
    };
    // (Anonymous member types have builder-assigned names, so the record is
    // compared member by member.)
    let (version, _) = schema_for::<Version>().unwrap();
    for schema in [&version, &recorded] {
        let AnyValue::Custom(custom) = &named(schema, "Version").value else { panic!("Custom") };
        assert_eq!(custom.id, "Version");
        let AnyValue::Object(record) = &custom.r#type.value else { panic!("Object") };
        let members: Vec<_> = record
            .members
            .iter()
            .map(|m| (m.name.as_str(), schema.resolve(&m.r#type).unwrap().clone(), m.attributes.len()))
            .collect();
        let u16_type = AnyType::new(AnyValue::Int(psio::pssz::schema_ir::Int { bits: 16, isSigned: false }));
        assert_eq!(members, [("major", u16_type.clone(), 0), ("minor", u16_type, 0)]);
        assert!(record.attributes.is_empty());
    }
    let (account, _) = schema_for::<Account>().unwrap();
    assert_eq!(named(&account, "Account"), named(&recorded, "Account"));
    let AnyValue::Custom(custom) = &named(&account, "Account").value else { panic!("Custom") };
    assert_eq!(custom.id, "AccountName");
    assert_eq!(Version::custom_id().as_deref(), Some("Version"));
    assert_eq!(Account::custom_id().as_deref(), Some("AccountName"));
    assert_eq!(Release::custom_id(), None);

    let handlers = typed_handlers();
    let mut strict = dynamic::CustomHandlers::new();
    strict.set_strict(true);
    let mut partial = typed_handlers_of_version_only();
    partial.set_strict(true);
    const NO_HANDLER: &str = "pssz dynamic: no handler registered for a custom type";

    fn check<T: Packed + ToPsszSchema>(
        row: &Row,
        handlers: &dynamic::CustomHandlers,
        strict: &dynamic::CustomHandlers,
    ) -> usize {
        let name = &row.name;
        let (schema, root) = schema_for::<T>().unwrap();
        let (Some(json_text), Some(ijson_text), Some(pssz)) = (&row.json, &row.ijson, &row.pssz)
        else {
            panic!("{name}: value row")
        };
        let bytes = unhex(pssz);
        let plain_json = row.plain_json.as_ref().expect("plain_json");
        let plain_ijson = row.plain_ijson.as_ref().expect("plain_ijson");
        // With handlers: the typed text from the bytes, the bytes from it.
        assert_eq!(
            dynamic::pssz_to_json_with::<Json>(&schema, &root, &bytes, handlers).unwrap().0,
            *json_text,
            "{name}"
        );
        assert_eq!(
            dynamic::pssz_to_json_with::<IJson>(&schema, &root, &bytes, handlers).unwrap().0,
            *ijson_text,
            "{name}"
        );
        assert_eq!(
            dynamic::json_to_pssz_with::<Json>(&schema, &root, json_text.as_bytes(), handlers).unwrap(),
            bytes,
            "{name}"
        );
        assert_eq!(
            dynamic::json_to_pssz_with::<IJson>(&schema, &root, ijson_text.as_bytes(), handlers).unwrap(),
            bytes,
            "{name}"
        );
        // Without: the wire type's own text, which reads back to the bytes.
        assert_eq!(dynamic::pssz_to_json::<Json>(&schema, &root, &bytes).unwrap().0, *plain_json, "{name}");
        assert_eq!(dynamic::pssz_to_json::<IJson>(&schema, &root, &bytes).unwrap().0, *plain_ijson, "{name}");
        assert_eq!(dynamic::json_to_pssz::<Json>(&schema, &root, plain_json.as_bytes()).unwrap(), bytes, "{name}");
        assert_eq!(dynamic::json_to_pssz::<IJson>(&schema, &root, plain_ijson.as_bytes()).unwrap(), bytes, "{name}");
        // An empty registry is the same fallback.
        let empty = dynamic::CustomHandlers::new();
        assert_eq!(
            dynamic::pssz_to_json_with::<Json>(&schema, &root, &bytes, &empty).unwrap().0,
            *plain_json
        );
        // With a handler the plain form is not also accepted (unless the two
        // are the same text, which no row of these types is).
        assert_ne!(plain_json, json_text, "{name}");
        assert!(dynamic::json_to_pssz_with::<Json>(&schema, &root, plain_json.as_bytes(), handlers).is_err(), "{name}");
        assert!(dynamic::json_to_pssz_with::<IJson>(&schema, &root, plain_ijson.as_bytes(), handlers).is_err(), "{name}");
        // Strict: a missing handler is an error in both directions.
        for error in [
            dynamic::pssz_to_json_with::<Json>(&schema, &root, &bytes, strict).unwrap_err(),
            dynamic::pssz_to_json_with::<IJson>(&schema, &root, &bytes, strict).unwrap_err(),
            dynamic::json_to_pssz_with::<Json>(&schema, &root, json_text.as_bytes(), strict).unwrap_err(),
            dynamic::json_to_pssz_with::<IJson>(&schema, &root, plain_ijson.as_bytes(), strict).unwrap_err(),
        ] {
            assert_eq!(error.what(), NO_HANDLER, "{name}");
        }
        // The binary side never needs a handler.
        assert_eq!(dynamic::validate_pssz(&schema, &root, &bytes), Ok(false), "{name}");
        1
    }
    let mut rows = 0usize;
    for row in fixture.vectors.iter().filter(|row| row.kind == "value") {
        rows += match row.ty.as_str() {
            "Version" => check::<Version>(row, &handlers, &strict),
            "Release" => check::<Release>(row, &handlers, &strict),
            "Account" => check::<Account>(row, &handlers, &strict),
            "Transfer" => check::<Transfer>(row, &handlers, &strict),
            _ => 0,
        };
    }
    assert!(rows >= 15, "custom value rows: {rows}");

    // A strict registry that has every handler the type needs is satisfied;
    // one that lacks any of them is not.
    let (schema, root) = schema_for::<Transfer>().unwrap();
    let transfer = Transfer {
        from: Account::from_text("alice").unwrap(),
        to: Account::from_text("bob").unwrap(),
        amount: 7,
        payer: None,
        notify: vec![],
        since: Some(Version { major: 1, minor: 2 }),
    };
    let bytes = psio::pssz::try_to_pssz::<Pssz, _>(&transfer).unwrap();
    let mut all = typed_handlers();
    all.set_strict(true);
    assert_eq!(
        dynamic::pssz_to_json_with::<IJson>(&schema, &root, &bytes, &all).unwrap().0,
        json::to_string::<IJson, _>(&transfer).unwrap()
    );
    assert_eq!(
        dynamic::pssz_to_json_with::<IJson>(&schema, &root, &bytes, &partial).unwrap_err().what(),
        NO_HANDLER
    );
    // Not strict: the handled ids are presented, the others plain.
    partial.set_strict(false);
    assert_eq!(
        dynamic::pssz_to_json_with::<Json>(&schema, &root, &bytes, &partial).unwrap().0,
        r#"{"from":235814801429757952,"to":274965867874222080,"amount":7,"payer":null,"notify":[],"since":"1.2"}"#
    );
    // A handler's errors are errors of the conversion.
    let mut failing = dynamic::CustomHandlers::new();
    failing.add(
        "AccountName",
        |_bytes: &[u8]| Err(psio::pssz::PsszError("no text")),
        |_text: &str| Ok(vec![1, 2, 3]), // not a u64
    );
    assert_eq!(
        dynamic::pssz_to_json_with::<Json>(&schema, &root, &bytes, &failing).unwrap_err().what(),
        "json: custom handler has no text for the value"
    );
    let text = json::to_string::<Json, _>(&transfer).unwrap();
    assert_eq!(
        dynamic::json_to_pssz_with::<Json>(&schema, &root, text.as_bytes(), &failing).unwrap_err().what(),
        "json: custom handler produced bytes that are not the inner type"
    );
    // A type that is not declared custom has no id to register under.
    assert!(dynamic::CustomHandlers::new().add_type::<Version>().is_ok());
    assert!(dynamic::CustomHandlers::new().contains("Version") == false);
    assert!(typed_handlers().contains("AccountName") && !typed_handlers().contains("Account"));
}

fn typed_handlers_of_version_only() -> dynamic::CustomHandlers {
    let mut handlers = dynamic::CustomHandlers::new();
    handlers.add_type::<Version>().unwrap();
    handlers
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
