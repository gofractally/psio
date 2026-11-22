// The PSIO derives read `#[serde(...)]`: an attribute with a PSIO meaning is
// honoured by every derive it concerns, and the binary derives ignore one
// that concerns text only. The attributes a derive rejects are compile_fail
// doctests in src/serde_attrs.rs.

use psio::pssz::dynamic;
use psio::pssz_schema_ir::{schema_for, ToPsszSchema};
use psio::{IJson, Json, JsonText, Key, PsszPack, PsszUnpack, PsszView, ToPsszSchema};
use std::marker::PhantomData;

/// The typed text of `value` in both formats, which the schema-driven codec
/// must also produce from the value's bytes and read back to the same bytes.
fn texts<T>(value: &T) -> (String, String)
where
    T: psio::json::JsonText + ToPsszSchema + psio::pssz::PsszPack<psio::Pssz> + PartialEq + std::fmt::Debug,
{
    let (schema, root) = schema_for::<T>().unwrap();
    let bytes = psio::to_pssz(value);
    let json = psio::to_json(value);
    let ijson = psio::to_ijson(value);
    assert_eq!(dynamic::pssz_to_json::<Json>(&schema, &root, &bytes).unwrap().0, json);
    assert_eq!(dynamic::pssz_to_json::<IJson>(&schema, &root, &bytes).unwrap().0, ijson);
    assert_eq!(dynamic::json_to_pssz::<Json>(&schema, &root, json.as_bytes()).unwrap(), bytes);
    assert_eq!(dynamic::json_to_pssz::<IJson>(&schema, &root, ijson.as_bytes()).unwrap(), bytes);
    assert_eq!(&psio::from_json::<T>(&json).unwrap(), value);
    assert_eq!(&psio::from_ijson::<T>(&ijson).unwrap(), value);
    (json, ijson)
}

// ── rename, rename_all ──────────────────────────────────────────────────────

#[derive(
    Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, JsonText, ToPsszSchema, Key,
    serde::Serialize, serde::Deserialize,
)]
#[serde(rename = "AccountV1", rename_all = "camelCase")]
struct Account {
    account_id: u32,
    #[serde(rename = "type")]
    kind: u8,
    display_name: String,
}

// The same members with no attribute: the bytes and the key are positional.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, ToPsszSchema, Key)]
struct PlainAccount {
    account_id: u32,
    kind: u8,
    display_name: String,
}

#[test]
fn rename_names_the_member_in_text_and_schema_not_on_the_wire() {
    let account = Account { account_id: 7, kind: 2, display_name: "alice".into() };
    let plain = PlainAccount { account_id: 7, kind: 2, display_name: "alice".into() };
    let (json, ijson) = texts(&account);
    assert_eq!(json, r#"{"accountId":7,"type":2,"displayName":"alice"}"#);
    assert_eq!(ijson, json);
    // serde's own derive reads the attributes the same way.
    assert_eq!(serde_json::to_string(&account).unwrap(), json);
    assert_eq!(serde_json::from_str::<Account>(&json).unwrap(), account);
    // The Rust names are not names of the type.
    assert!(psio::from_json::<Account>(r#"{"account_id":7,"kind":2,"display_name":"a"}"#).is_err());
    // pSSZ and the key are positional.
    assert_eq!(psio::to_pssz(&account), psio::to_pssz(&plain));
    assert_eq!(psio::to_key(&account), psio::to_key(&plain));
    assert_eq!(psio::from_pssz::<Account>(&psio::to_pssz(&plain)).unwrap(), account);
    assert_eq!(psio::from_key::<Account>(&psio::to_key(&plain)).unwrap(), account);
    // A view's accessors are the Rust identifiers.
    let bytes = psio::to_pssz(&account);
    let view = psio::PsszView::<Account>::new(&bytes).unwrap();
    assert_eq!(view.account_id().get(), 7);
    assert_eq!(view.kind().get(), 2);
    assert_eq!(view.display_name().view().unwrap(), "alice");
    // The container's name is the schema type name.
    assert_eq!(Account::schema_name().as_deref(), Some("AccountV1"));
    let (schema, _) = schema_for::<Account>().unwrap();
    let text = psio::to_ijson(&schema);
    assert!(text.contains(r#""name":"AccountV1""#), "{text}");
    assert!(text.contains(r#""name":"accountId""#), "{text}");
    assert!(text.contains(r#""name":"type""#), "{text}");
    assert!(!text.contains("account_id"), "{text}");
}

// The neutral spelling.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, ToPsszSchema, Key)]
#[psio(name = "AccountV1", rename_all = "camelCase")]
struct PsioAccount {
    account_id: u32,
    #[psio(name = "type")]
    kind: u8,
    display_name: String,
}

#[test]
fn psio_name_and_rename_all_say_the_same() {
    let account = PsioAccount { account_id: 7, kind: 2, display_name: "alice".into() };
    let (json, _) = texts(&account);
    assert_eq!(json, r#"{"accountId":7,"type":2,"displayName":"alice"}"#);
    assert_eq!(PsioAccount::schema_name().as_deref(), Some("AccountV1"));
}

macro_rules! cased {
    ($name:ident, $case:literal) => {
        #[derive(
            Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, ToPsszSchema,
            serde::Serialize, serde::Deserialize,
        )]
        #[serde(rename_all = $case)]
        struct $name {
            first_name: u8,
            http_status2_code: u8,
            x: u8,
        }
    };
}
cased!(Lower, "lowercase");
cased!(Upper, "UPPERCASE");
cased!(Pascal, "PascalCase");
cased!(Camel, "camelCase");
cased!(Snake, "snake_case");
cased!(ScreamingSnake, "SCREAMING_SNAKE_CASE");
cased!(Kebab, "kebab-case");
cased!(ScreamingKebab, "SCREAMING-KEBAB-CASE");

#[test]
fn every_member_case_convention_is_serdes() {
    macro_rules! check {
        ($name:ident, $text:literal) => {{
            let value = $name { first_name: 1, http_status2_code: 2, x: 3 };
            let (json, _) = texts(&value);
            assert_eq!(json, $text);
            assert_eq!(serde_json::to_string(&value).unwrap(), json);
        }};
    }
    check!(Lower, r#"{"first_name":1,"http_status2_code":2,"x":3}"#);
    check!(Upper, r#"{"FIRST_NAME":1,"HTTP_STATUS2_CODE":2,"X":3}"#);
    check!(Pascal, r#"{"FirstName":1,"HttpStatus2Code":2,"X":3}"#);
    check!(Camel, r#"{"firstName":1,"httpStatus2Code":2,"x":3}"#);
    check!(Snake, r#"{"first_name":1,"http_status2_code":2,"x":3}"#);
    check!(ScreamingSnake, r#"{"FIRST_NAME":1,"HTTP_STATUS2_CODE":2,"X":3}"#);
    check!(Kebab, r#"{"first-name":1,"http-status2-code":2,"x":3}"#);
    check!(ScreamingKebab, r#"{"FIRST-NAME":1,"HTTP-STATUS2-CODE":2,"X":3}"#);
}

// A field-less enum: the enumerator names.
#[derive(
    Debug, Clone, Copy, PartialEq, PsszPack, PsszUnpack, JsonText, ToPsszSchema, Key,
    serde::Serialize, serde::Deserialize,
)]
#[repr(u8)]
#[serde(rename_all = "snake_case")]
enum Status {
    NotStarted = 0,
    InProgress = 1,
    #[serde(rename = "DONE")]
    Finished = 2,
}

// An enum with payloads: the alternative names.
#[derive(
    Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, ToPsszSchema, Key,
    serde::Serialize, serde::Deserialize,
)]
#[serde(rename_all = "SCREAMING-KEBAB-CASE")]
enum Shape {
    UnitSquare(u8),
    #[serde(rename = "circle")]
    Circle(u32),
    LongRect(u16),
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack)]
enum PlainShape {
    UnitSquare(u8),
    Circle(u32),
    LongRect(u16),
}

#[test]
fn rename_all_names_the_variants_of_an_enum() {
    for (value, name) in [
        (Status::NotStarted, r#""not_started""#),
        (Status::InProgress, r#""in_progress""#),
        (Status::Finished, r#""DONE""#),
    ] {
        let (json, ijson) = texts(&value);
        assert_eq!(json, name);
        assert_eq!(ijson, name);
        assert_eq!(serde_json::to_string(&value).unwrap(), name);
        assert_eq!(psio::to_pssz(&value), [value as u8]);
        assert_eq!(psio::to_key(&value), [value as u8]);
    }
    assert!(psio::from_json::<Status>(r#""NotStarted""#).is_err());

    for (value, plain, ijson_text, json_text) in [
        (Shape::UnitSquare(1), PlainShape::UnitSquare(1), r#"{"UNIT-SQUARE":1}"#, "[0,1]"),
        (Shape::Circle(2), PlainShape::Circle(2), r#"{"circle":2}"#, "[1,2]"),
        (Shape::LongRect(3), PlainShape::LongRect(3), r#"{"LONG-RECT":3}"#, "[2,3]"),
    ] {
        let (json, ijson) = texts(&value);
        assert_eq!(ijson, ijson_text);
        assert_eq!(json, json_text);
        // serde's externally tagged form is PSIO's IJson form.
        assert_eq!(serde_json::to_string(&value).unwrap(), ijson_text);
        assert_eq!(psio::to_pssz(&value), psio::to_pssz(&plain));
    }
    assert!(psio::from_ijson::<Shape>(r#"{"UnitSquare":1}"#).is_err());
}

// Struct variants have no typed JSON; the schema names their members.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema, serde::Serialize)]
#[serde(rename_all_fields = "camelCase")]
enum Event {
    Opened { opened_by: u32 },
    #[serde(rename_all = "UPPERCASE")]
    Closed { closed_by: u32, exit_code: u8 },
}

#[test]
fn struct_variant_members_follow_rename_all_fields_and_the_variants_rename_all() {
    let (schema, _) = schema_for::<Event>().unwrap();
    let text = psio::to_ijson(&schema);
    assert!(text.contains(r#""name":"openedBy""#), "{text}");
    assert!(text.contains(r#""name":"CLOSED_BY""#), "{text}");
    assert!(text.contains(r#""name":"EXIT_CODE""#), "{text}");
    assert!(!text.contains("opened_by"), "{text}");
}

// ── skip ────────────────────────────────────────────────────────────────────

fn seven() -> u32 {
    7
}

#[derive(
    Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, JsonText, ToPsszSchema, Key,
    serde::Serialize, serde::Deserialize,
)]
struct Cached {
    id: u32,
    #[serde(skip)]
    hits: u64,
    name: String,
    #[psio(skip)]
    #[serde(skip)]
    scratch: Vec<u8>,
    #[serde(skip, default = "seven")]
    generation: u32,
    #[serde(skip_serializing, skip_deserializing)]
    dirty: bool,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, ToPsszSchema, Key)]
struct Uncached {
    id: u32,
    name: String,
}

#[test]
fn a_skipped_field_is_in_no_format_and_reads_as_its_default() {
    let cached = Cached {
        id: 5,
        hits: 99,
        name: "n".into(),
        scratch: vec![1, 2, 3],
        generation: 1,
        dirty: true,
    };
    let plain = Uncached { id: 5, name: "n".into() };
    let read = Cached { id: 5, hits: 0, name: "n".into(), scratch: vec![], generation: 7, dirty: false };

    // Text: the two members, in both formats and through the schema.
    assert_eq!(psio::to_json(&cached), r#"{"id":5,"name":"n"}"#);
    assert_eq!(psio::to_ijson(&cached), psio::to_ijson(&plain));
    assert_eq!(psio::from_json::<Cached>(r#"{"id":5,"name":"n"}"#).unwrap(), read);
    texts(&read);
    // A skipped field's name is an unknown member, which a reader skips.
    assert_eq!(
        psio::from_json::<Cached>(r#"{"id":5,"hits":"not a number","name":"n"}"#).unwrap(),
        read
    );
    // serde's own derive agrees.
    assert_eq!(serde_json::to_string(&cached).unwrap(), r#"{"id":5,"name":"n"}"#);
    assert_eq!(serde_json::from_str::<Cached>(r#"{"id":5,"name":"n"}"#).unwrap(), read);

    // pSSZ: the bytes, the layout constants and the shape of the type
    // without the fields.
    assert_eq!(psio::to_pssz(&cached), psio::to_pssz(&plain));
    assert_eq!(psio::from_pssz::<Cached>(&psio::to_pssz(&plain)).unwrap(), read);
    psio::validate_pssz::<Cached>(&psio::to_pssz(&plain)).unwrap();
    assert_eq!(
        <Cached as psio::pssz::PsszPack<psio::Pssz>>::MIN_ENCODED_SIZE,
        <Uncached as psio::pssz::PsszPack<psio::Pssz>>::MIN_ENCODED_SIZE
    );
    assert!(psio::pssz::pssz_evolves_from::<Cached, Uncached>());
    assert!(psio::pssz::pssz_evolves_from::<Uncached, Cached>());

    // Key.
    assert_eq!(psio::to_key(&cached), psio::to_key(&plain));
    assert_eq!(psio::from_key::<Cached>(&psio::to_key(&plain)).unwrap(), read);

    // Schema: the same members as the type without the fields.
    let members = |text: String| text.replace("Uncached", "Cached");
    assert_eq!(
        psio::to_ijson(&schema_for::<Cached>().unwrap().0),
        members(psio::to_ijson(&schema_for::<Uncached>().unwrap().0))
    );

    // View: accessors for the members, numbered as on the wire.
    let bytes = psio::to_pssz(&cached);
    let view = psio::PsszView::<Cached>::new(&bytes).unwrap();
    assert_eq!(view.id().get(), 5);
    assert_eq!(view.name().view().unwrap(), "n");
    assert_eq!(view.field::<1>().view().unwrap(), "n");
}

// A skipped field's type needs no codec, only `Default`.
#[derive(Debug, Default, Clone, PartialEq)]
struct NoCodec(std::collections::HashMap<u8, f32>);

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, JsonText, ToPsszSchema, Key)]
#[psio(transparent)]
struct Tagged<T> {
    value: u64,
    #[psio(skip)]
    cache: NoCodec,
    #[psio(skip)]
    unit: PhantomData<T>,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema, Key)]
struct Pair(u16, #[psio(skip)] NoCodec, u16);

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, Key)]
struct PlainPair(u16, u16);

#[derive(Debug, Clone, PartialEq, JsonText)]
struct Newtype(#[psio(skip)] NoCodec, String);

#[test]
fn transparent_and_tuple_structs_count_members_not_fields() {
    let tagged = Tagged::<u8> { value: 9, cache: NoCodec::default(), unit: PhantomData };
    assert_eq!(psio::to_pssz(&tagged), psio::to_pssz(&9u64));
    assert_eq!(psio::to_json(&tagged), "9");
    assert_eq!(psio::to_key(&tagged), psio::to_key(&9u64));
    assert_eq!(psio::from_pssz::<Tagged<u8>>(&psio::to_pssz(&9u64)).unwrap(), tagged);
    assert_eq!(psio::from_json::<Tagged<u8>>("9").unwrap(), tagged);
    assert_eq!(psio::from_key::<Tagged<u8>>(&psio::to_key(&9u64)).unwrap(), tagged);

    let pair = Pair(1, NoCodec::default(), 2);
    assert_eq!(psio::to_pssz(&pair), psio::to_pssz(&PlainPair(1, 2)));
    assert_eq!(psio::from_pssz::<Pair>(&psio::to_pssz(&pair)).unwrap(), pair);
    assert_eq!(psio::to_key(&pair), psio::to_key(&PlainPair(1, 2)));
    assert_eq!(psio::from_key::<Pair>(&psio::to_key(&pair)).unwrap(), pair);

    let newtype = Newtype(NoCodec::default(), "x".into());
    assert_eq!(psio::to_json(&newtype), r#""x""#);
    assert_eq!(psio::from_json::<Newtype>(r#""x""#).unwrap(), newtype);
}

// ── Accepted with no effect ─────────────────────────────────────────────────

#[derive(
    Debug, Clone, PartialEq, PsszPack, PsszUnpack, JsonText, ToPsszSchema,
    serde::Serialize, serde::Deserialize,
)]
struct Profile {
    id: u32,
    #[serde(default)]
    nickname: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    age: Option<u8>,
}

#[test]
fn a_missing_optional_member_is_none_and_none_is_written_as_null() {
    let profile = Profile { id: 1, nickname: None, age: None };
    let (json, _) = texts(&profile);
    // PSIO writes every member. serde_json, told to skip a None, omits it.
    assert_eq!(json, r#"{"id":1,"nickname":null,"age":null}"#);
    assert_eq!(serde_json::to_string(&profile).unwrap(), r#"{"id":1,"nickname":null}"#);
    // Both read either text.
    for text in [r#"{"id":1}"#, r#"{"id":1,"nickname":null}"#, json.as_str()] {
        assert_eq!(psio::from_json::<Profile>(text).unwrap(), profile);
        assert_eq!(serde_json::from_str::<Profile>(text).unwrap(), profile);
    }
}

// ── The binary derives ignore what concerns text only ───────────────────────

fn is_zero(n: &u32) -> bool {
    *n == 0
}
mod as_text {
    pub fn serialize<S: serde::Serializer>(n: &u64, s: S) -> Result<S::Ok, S::Error> {
        s.collect_str(n)
    }
    pub fn deserialize<'de, D: serde::Deserializer<'de>>(d: D) -> Result<u64, D::Error> {
        let text: String = serde::Deserialize::deserialize(d)?;
        text.parse().map_err(serde::de::Error::custom)
    }
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, Key, serde::Serialize, serde::Deserialize)]
struct Inner {
    a: u8,
    b: u8,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, Key, serde::Serialize, serde::Deserialize)]
#[serde(deny_unknown_fields, default, bound = "", expecting = "a record", tag = "kind")]
struct TextOnly {
    #[serde(rename(serialize = "out", deserialize = "in"))]
    id: u32,
    #[serde(default, alias = "n", skip_serializing_if = "is_zero")]
    count: u32,
    #[serde(with = "as_text")]
    big: u64,
    #[serde(default = "seven")]
    generation: u32,
    inner: Inner,
}
impl Default for TextOnly {
    fn default() -> Self {
        TextOnly { id: 0, count: 0, big: 0, generation: 0, inner: Inner { a: 0, b: 0 } }
    }
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, Key)]
struct PlainTextOnly {
    id: u32,
    count: u32,
    big: u64,
    generation: u32,
    inner: Inner,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, Key, serde::Serialize, serde::Deserialize)]
struct Flat {
    id: u32,
    #[serde(flatten)]
    inner: Inner,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, Key)]
struct PlainFlat {
    id: u32,
    inner: Inner,
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, PsszView, Key, serde::Serialize, serde::Deserialize)]
#[serde(tag = "t", content = "c")]
enum Tagged2 {
    #[serde(alias = "one")]
    A(u8),
    #[serde(skip_serializing)]
    B(u16),
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, Key, serde::Serialize, serde::Deserialize)]
#[serde(untagged)]
enum Untagged {
    A(u8),
    B(String),
}

#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, Key, serde::Serialize, serde::Deserialize)]
#[serde(into = "u64", from = "u64")]
struct Converted {
    hi: u32,
    lo: u32,
}
impl From<Converted> for u64 {
    fn from(c: Converted) -> u64 {
        (u64::from(c.hi) << 32) | u64::from(c.lo)
    }
}
impl From<u64> for Converted {
    fn from(n: u64) -> Self {
        Converted { hi: (n >> 32) as u32, lo: n as u32 }
    }
}

#[test]
fn binary_derives_ignore_text_only_attributes() {
    let value = TextOnly { id: 1, count: 2, big: 3, generation: 4, inner: Inner { a: 5, b: 6 } };
    let plain =
        PlainTextOnly { id: 1, count: 2, big: 3, generation: 4, inner: Inner { a: 5, b: 6 } };
    assert_eq!(psio::to_pssz(&value), psio::to_pssz(&plain));
    assert_eq!(psio::to_key(&value), psio::to_key(&plain));
    assert_eq!(psio::from_pssz::<TextOnly>(&psio::to_pssz(&plain)).unwrap(), value);
    assert_eq!(psio::from_key::<TextOnly>(&psio::to_key(&plain)).unwrap(), value);

    // A flattened member is a member of its own type on the wire.
    let flat = Flat { id: 1, inner: Inner { a: 2, b: 3 } };
    let plain = PlainFlat { id: 1, inner: Inner { a: 2, b: 3 } };
    assert_eq!(psio::to_pssz(&flat), psio::to_pssz(&plain));
    assert_eq!(psio::to_key(&flat), psio::to_key(&plain));
    assert_eq!(serde_json::to_string(&flat).unwrap(), r#"{"id":1,"a":2,"b":3}"#);

    // Tagging is a property of serde's text: the selector is the index.
    assert_eq!(psio::to_pssz(&Tagged2::B(7)), [1, 7, 0]);
    assert_eq!(psio::to_key(&Tagged2::A(7)), [0, 7]);
    assert_eq!(psio::to_pssz(&Untagged::A(7)), [0, 7]);
    assert_eq!(psio::from_pssz::<Untagged>(&[0, 7]).unwrap(), Untagged::A(7));

    // A conversion type is serde's representation; the bytes are the fields'.
    let converted = Converted { hi: 1, lo: 2 };
    assert_eq!(psio::to_pssz(&converted), [8, 1, 0, 0, 0, 2, 0, 0, 0]);
    assert_eq!(serde_json::to_string(&converted).unwrap(), "4294967298");
}

// A type with a conversion for serde and a text form for PSIO: the schema
// says the type has a text of its own.
#[derive(Debug, Clone, PartialEq, PsszPack, PsszUnpack, ToPsszSchema, serde::Serialize, serde::Deserialize)]
#[psio(transparent, custom)]
#[serde(into = "String", try_from = "String")]
struct Version {
    packed: u16,
}
impl psio::json::TextForm for Version {
    fn to_text(&self, out: &mut String) {
        out.push_str(&format!("{}.{}", self.packed >> 8, self.packed & 0xff));
    }
    fn from_text(text: &str) -> Option<Self> {
        let (major, minor) = text.split_once('.')?;
        Some(Version { packed: (major.parse::<u8>().ok()? as u16) << 8 | minor.parse::<u8>().ok()? as u16 })
    }
}
psio::json_text_form!(Version);
impl From<Version> for String {
    fn from(v: Version) -> String {
        let mut out = String::new();
        psio::json::TextForm::to_text(&v, &mut out);
        out
    }
}
impl TryFrom<String> for Version {
    type Error = &'static str;
    fn try_from(text: String) -> Result<Self, Self::Error> {
        psio::json::TextForm::from_text(&text).ok_or("not a version")
    }
}

#[test]
fn a_conversion_for_serde_pairs_with_a_text_form_for_psio() {
    let version = Version { packed: 0x0102 };
    assert_eq!(psio::to_json(&version), r#""1.2""#);
    assert_eq!(serde_json::to_string(&version).unwrap(), r#""1.2""#);
    assert_eq!(psio::to_pssz(&version), [2, 1]);
    assert_eq!(Version::custom_id().as_deref(), Some("Version"));
}
