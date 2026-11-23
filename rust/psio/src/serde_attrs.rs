//! How the PSIO derives read `#[serde(...)]`.
//!
//! A type annotated for serde is described once: the PSIO derives read the
//! same attributes. There are three outcomes for an attribute.
//!
//! * **Honoured.** It has a PSIO meaning and every derive it concerns
//!   applies it. Each has a neutral spelling under `#[psio(...)]`.
//! * **Error.** PSIO's text, or what PSIO accepts, would differ from what
//!   the attribute says. The derives that describe text reject it:
//!   `JsonText`, `ToPsszSchema` (its schema drives the schema-based JSON
//!   codec) and `Serde` ([`serde_bridge`](crate::serde_bridge)). PSIO has one text per type, the same in every
//!   language, so a different text is never produced silently.
//! * **Ignored.** It only steers the code serde generates, or it concerns
//!   text and the derive is a binary one (`PsszPack`, `PsszUnpack`,
//!   `PsszView`, `Key`).
//!
//! The derives declare `serde` as a helper attribute, so `#[serde(...)]` may
//! be written on a type that uses only PSIO derives.
//!
//! # Container attributes
//!
//! | serde | PSIO |
//! |-------|------|
//! | `rename = "N"` | Honoured: the type's name in a schema, as `#[psio(name = "N")]`. No effect on text or bytes |
//! | `rename(serialize = "A", deserialize = "B")` | Error on the text derives when the names differ: text has one name |
//! | `rename_all = "case"` | Honoured, as `#[psio(rename_all = "case")]`: the member names of a struct, or the variant names of an enum, in text and in a schema. Every serde convention, applied as serde applies it. Different conventions for the two directions are an error on the text derives |
//! | `rename_all_fields = "case"` | Honoured by `ToPsszSchema`: the member names of every struct variant. (`JsonText` has no struct variants) |
//! | `deny_unknown_fields` | Error on the text derives: a PSIO reader skips unknown members |
//! | `untagged` (on an enum whose variants hold values) | Honoured, as `#[psio(untagged)]`: the text is the bare value of the alternative in both formats, the schema's `Variant` carries the attribute `untagged`, and a reader picks the alternative by the kind of JSON value it finds (docs/json.md "Untagged variants"), not by trying each as serde does. Two alternatives written as the same kind are a compile error |
//! | `tag = "t"`, `tag = "t", content = "c"` | Error on the text derives: a variant is `{"Name": value}` under IJson, which is serde's default externally tagged form, and `[index, value]` under Json, or untagged |
//! | `bound = "..."` | Ignored |
//! | `default`, `default = "path"` | Error on the text derives: a missing member is an error unless it is an `Option` |
//! | `remote = "..."` | Ignored: the derives describe the type they are written on |
//! | `transparent` | Honoured by every derive, as `#[psio(transparent)]`: the type is its one member |
//! | `from = "T"`, `try_from = "T"`, `into = "T"` | Error on `JsonText` and `Serde`, and on `ToPsszSchema` unless the type is `#[psio(custom)]`: the text would be the conversion type's. A type with a text of its own has a [`TextForm`](crate::json::TextForm) |
//! | `crate = "..."`, `expecting = "..."` | Ignored |
//! | `variant_identifier`, `field_identifier` | Error on the text derives: PSIO reads an enumerator from its name, its number, the numeric string or `"name:number"` |
//!
//! # Variant attributes
//!
//! | serde | PSIO |
//! |-------|------|
//! | `rename = "N"` | Honoured, as `#[psio(name = "N")]`: the enumerator's or alternative's name in text and in a schema. Different names for the two directions are an error on the text derives |
//! | `alias = "N"` | Error on the text derives: text has one name |
//! | `rename_all = "case"` | Honoured by `ToPsszSchema`: the member names of this struct variant |
//! | `skip` | Error on every derive: each variant is an alternative with its own index |
//! | `skip_serializing`, `skip_deserializing` | Error on the text derives |
//! | `serialize_with`, `deserialize_with`, `with` | Error on the text derives: the text is defined by the payload's type |
//! | `bound`, `borrow` | Ignored |
//! | `other` | Error on the text derives: serde's catch-all drops the tag. PSIO's is `#[psio(other)] Unknown(<repr>)`, which keeps the number |
//! | `untagged` (on one variant) | Error on the text derives: a type is untagged as a whole |
//!
//! # Field attributes
//!
//! | serde | PSIO |
//! |-------|------|
//! | `rename = "N"` | Honoured, as `#[psio(name = "N")]`: the member's name in text and in a schema. pSSZ and keys are positional and unaffected; a view's accessor keeps the Rust identifier. Different names for the two directions are an error on the text derives |
//! | `alias = "N"` | Error on the text derives |
//! | `default` | Accepted on an `Option` member, where it is PSIO's rule (a missing optional member is `None`). Error on the text derives on any other member |
//! | `default = "path"` | Error on the text derives. On a skipped field: the value a reader fills it with |
//! | `flatten` | Error on the text derives: a member is one key of its record, in text and in the Schema IR. On the wire the member is a value of its own type |
//! | `skip` | Honoured by every derive, as `#[psio(skip)]`: the field is not a member of the type. It is not in the pSSZ bytes, the key, the text, the schema or the shape, and has no view accessor; a reader fills it with `Default::default()` |
//! | `skip_serializing`, `skip_deserializing` | Both together are `skip`. One alone is an error on every derive: a field is a member in both directions or in neither |
//! | `skip_serializing_if = "Option::is_none"` | Accepted, no effect: PSIO writes `null` and reads a missing member as `None`. `serde_json` with serde's own derive omits the member, so the two texts differ and each reads the other's. Any other predicate is an error on the text derives |
//! | `serialize_with`, `deserialize_with`, `with` | Error on the text derives |
//! | `borrow`, `bound`, `getter` | Ignored |
//!
//! The `Capnp*`, `Fb*`, `Wit*` and `ToDynamicSchema` derives treat every
//! field as a member: on them a skipped field is an error and the other
//! attributes are ignored.
//!
//! # What the text derives reject
//!
//! Each block fails to compile with the attribute and compiles without it,
//! or with the binary derives only.
//!
//! Two names for one member:
//!
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! struct Point {
//!     #[serde(rename(serialize = "x", deserialize = "X"))]
//!     x: u32,
//! }
//! ```
//! ```
//! #[derive(psio::JsonText, psio::PsszPack, psio::PsszUnpack)]
//! struct Point {
//!     #[serde(rename(serialize = "X", deserialize = "X"))]
//!     x: u32,
//! }
//! assert_eq!(psio::to_json(&Point { x: 1 }), r#"{"X":1}"#);
//!
//! #[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::Key)]
//! struct Wire {
//!     #[serde(rename(serialize = "x", deserialize = "X"))]
//!     x: u32,
//! }
//! ```
//!
//! Two conventions:
//!
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! #[serde(rename_all(serialize = "camelCase", deserialize = "snake_case"))]
//! struct Point { x_pos: u32 }
//! ```
//! ```
//! #[derive(psio::JsonText)]
//! #[serde(rename_all = "camelCase")]
//! struct Point { x_pos: u32 }
//!
//! #[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::Key)]
//! #[serde(rename_all(serialize = "camelCase", deserialize = "snake_case"))]
//! struct Wire { x_pos: u32 }
//! ```
//!
//! `#[psio(name)]` and `#[serde(rename)]` that disagree:
//!
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! struct Point {
//!     #[psio(name = "a")]
//!     #[serde(rename = "b")]
//!     x: u32,
//! }
//! ```
//! ```
//! #[derive(psio::JsonText)]
//! struct Point {
//!     #[psio(name = "a")]
//!     #[serde(rename = "a")]
//!     x: u32,
//! }
//! ```
//!
//! Two members with one name:
//!
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! struct Point {
//!     x: u32,
//!     #[serde(rename = "x")]
//!     y: u32,
//! }
//! ```
//!
//! An unknown convention:
//!
//! ```compile_fail
//! #[derive(psio::PsszPack)]
//! #[psio(rename_all = "Title Case")]
//! struct Point { x: u32 }
//! ```
//!
//! `deny_unknown_fields`:
//!
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! #[serde(deny_unknown_fields)]
//! struct Point { x: u32 }
//! ```
//! ```compile_fail
//! #[derive(psio::ToPsszSchema)]
//! #[serde(deny_unknown_fields)]
//! struct Point { x: u32 }
//! ```
//! ```
//! #[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::Key)]
//! #[serde(deny_unknown_fields)]
//! struct Point { x: u32 }
//! ```
//!
//! Tagging:
//!
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! #[serde(tag = "type")]
//! enum Shape { Circle(u32), Square(u32) }
//! ```
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! #[serde(tag = "t", content = "c")]
//! enum Shape { Circle(u32), Square(u32) }
//! ```
//! ```compile_fail
//! // Untagged, and both alternatives are written as a number.
//! #[derive(psio::JsonText)]
//! #[serde(untagged)]
//! enum Shape { Circle(u32), Square(u64) }
//! ```
//! ```
//! // Untagged: the alternatives are told apart by the kind of JSON value.
//! #[derive(Debug, PartialEq, psio::JsonText, psio::ToPsszSchema, psio::PsszPack, psio::PsszUnpack)]
//! #[serde(untagged)]
//! enum Extra { One(String), Many(Vec<String>) }
//! assert_eq!(psio::to_ijson(&Extra::One("s".into())), r#""s""#);
//! assert_eq!(psio::from_json::<Extra>(r#"["a"]"#).unwrap(), Extra::Many(vec!["a".into()]));
//! ```
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! enum Shape {
//!     Circle(u32),
//!     #[serde(untagged)]
//!     Square(u64),
//! }
//! ```
//! ```compile_fail
//! #[derive(psio::PsszPack, psio::ToPsszSchema)]
//! #[serde(tag = "type")]
//! enum Shape { Circle(u32), Square(u32) }
//! ```
//! ```
//! #[derive(psio::JsonText)]
//! enum Shape { Circle(u32), Square(u32) }
//! assert_eq!(psio::to_ijson(&Shape::Circle(1)), r#"{"Circle":1}"#);
//! assert_eq!(psio::to_json(&Shape::Square(1)), "[1,1]");
//!
//! #[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::Key)]
//! #[serde(tag = "t", content = "c")]
//! enum Wire { Circle(u32), Square(u32) }
//!
//! #[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::Key)]
//! #[serde(untagged)]
//! enum Bare {
//!     Circle(u32),
//!     #[serde(untagged)]
//!     Square(u64),
//! }
//! ```
//!
//! `Serde` gives the type PSIO's text through serde, so it rejects what
//! `JsonText` rejects:
//!
//! ```compile_fail
//! #[derive(psio::Serde)]
//! #[serde(deny_unknown_fields)]
//! struct Id(u32);
//! impl psio::json::TextForm for Id {
//!     fn to_text(&self, out: &mut String) { out.push_str(&self.0.to_string()) }
//!     fn from_text(text: &str) -> Option<Self> { text.parse().ok().map(Id) }
//! }
//! psio::json_text_form!(Id);
//! ```
//! ```
//! #[derive(psio::Serde)]
//! struct Id(u32);
//! impl psio::json::TextForm for Id {
//!     fn to_text(&self, out: &mut String) { out.push_str(&self.0.to_string()) }
//!     fn from_text(text: &str) -> Option<Self> { text.parse().ok().map(Id) }
//! }
//! psio::json_text_form!(Id);
//! assert_eq!(serde_json::to_string(&Id(7)).unwrap(), "\"7\"");
//! ```
//!
//! Defaults:
//!
//! ```compile_fail
//! #[derive(psio::JsonText, Default)]
//! #[serde(default)]
//! struct Point { x: u32 }
//! ```
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! struct Point {
//!     #[serde(default)]
//!     x: u32,
//! }
//! ```
//! ```compile_fail
//! fn one() -> Option<u32> { Some(1) }
//! #[derive(psio::JsonText)]
//! struct Point {
//!     #[serde(default = "one")]
//!     x: Option<u32>,
//! }
//! ```
//! ```compile_fail
//! #[derive(psio::PsszPack, psio::ToPsszSchema)]
//! struct Point {
//!     #[serde(default)]
//!     x: u32,
//! }
//! ```
//! ```
//! #[derive(psio::JsonText, Debug, PartialEq)]
//! struct Point {
//!     #[serde(default)]
//!     x: Option<u32>,
//! }
//! assert_eq!(psio::from_json::<Point>("{}")?, Point { x: None });
//!
//! fn one() -> u32 { 1 }
//! #[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::Key, Default)]
//! #[serde(default)]
//! struct Wire {
//!     #[serde(default)]
//!     x: u32,
//!     #[serde(default = "one")]
//!     y: u32,
//! }
//! # Ok::<(), psio::json::JsonTextError>(())
//! ```
//!
//! Conversions:
//!
//! ```compile_fail
//! #[derive(psio::JsonText, Clone)]
//! #[serde(into = "u32")]
//! struct Id { value: u32 }
//! impl From<Id> for u32 { fn from(id: Id) -> u32 { id.value } }
//! ```
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! #[serde(from = "u32")]
//! struct Id { value: u32 }
//! impl From<u32> for Id { fn from(value: u32) -> Id { Id { value } } }
//! ```
//! ```compile_fail
//! #[derive(psio::PsszPack, psio::ToPsszSchema)]
//! #[serde(try_from = "u32")]
//! struct Id { value: u32 }
//! impl TryFrom<u32> for Id {
//!     type Error = String;
//!     fn try_from(value: u32) -> Result<Id, String> { Ok(Id { value }) }
//! }
//! ```
//! ```
//! // The wire is the fields'; the schema says the type has its own text.
//! #[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::Key, psio::ToPsszSchema)]
//! #[psio(custom)]
//! #[serde(from = "u32", into = "u32")]
//! struct Id { value: u32 }
//! impl From<u32> for Id { fn from(value: u32) -> Id { Id { value } } }
//! impl From<Id> for u32 { fn from(id: Id) -> u32 { id.value } }
//! ```
//!
//! Identifier enums:
//!
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! #[repr(u8)]
//! #[serde(variant_identifier)]
//! enum Field { A = 0, B = 1 }
//! ```
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! #[repr(u8)]
//! #[serde(field_identifier)]
//! enum Field { A = 0, B = 1 }
//! ```
//! ```
//! #[derive(psio::JsonText)]
//! #[repr(u8)]
//! enum Field { A = 0, B = 1 }
//!
//! #[derive(psio::PsszPack, psio::PsszUnpack, psio::Key)]
//! #[repr(u8)]
//! #[serde(variant_identifier)]
//! enum Wire { A = 0, B = 1 }
//! ```
//!
//! Aliases:
//!
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! struct Point {
//!     #[serde(alias = "X")]
//!     x: u32,
//! }
//! ```
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! enum Shape {
//!     #[serde(alias = "Round")]
//!     Circle(u32),
//! }
//! ```
//! ```
//! #[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::Key)]
//! struct Point {
//!     #[serde(alias = "X")]
//!     x: u32,
//! }
//! #[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::Key)]
//! enum Shape {
//!     #[serde(alias = "Round")]
//!     Circle(u32),
//! }
//! ```
//!
//! Custom functions:
//!
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! struct Point {
//!     #[serde(with = "as_text")]
//!     x: u32,
//! }
//! ```
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! struct Point {
//!     #[serde(serialize_with = "write_x")]
//!     x: u32,
//! }
//! ```
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! struct Point {
//!     #[serde(deserialize_with = "read_x")]
//!     x: u32,
//! }
//! ```
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! enum Shape {
//!     #[serde(with = "as_text")]
//!     Circle(u32),
//! }
//! ```
//! ```
//! #[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::Key)]
//! struct Point {
//!     #[serde(with = "as_text")]
//!     x: u32,
//!     #[serde(serialize_with = "write_y", deserialize_with = "read_y")]
//!     y: u32,
//! }
//! ```
//!
//! Flattening:
//!
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! struct Inner { a: u32 }
//! #[derive(psio::JsonText)]
//! struct Outer {
//!     id: u32,
//!     #[serde(flatten)]
//!     inner: Inner,
//! }
//! ```
//! ```compile_fail
//! #[derive(psio::PsszPack, psio::ToPsszSchema)]
//! struct Inner { a: u32 }
//! #[derive(psio::PsszPack, psio::ToPsszSchema)]
//! struct Outer {
//!     id: u32,
//!     #[serde(flatten)]
//!     inner: Inner,
//! }
//! ```
//! ```
//! #[derive(psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::Key)]
//! struct Inner { a: u32 }
//! #[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::Key)]
//! struct Outer {
//!     id: u32,
//!     #[serde(flatten)]
//!     inner: Inner,
//! }
//! ```
//!
//! Skipping in one direction, on every derive:
//!
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! struct Login {
//!     user: String,
//!     #[serde(skip_serializing)]
//!     password: String,
//! }
//! ```
//! ```compile_fail
//! #[derive(psio::PsszPack)]
//! struct Login {
//!     user: String,
//!     #[serde(skip_serializing)]
//!     password: String,
//! }
//! ```
//! ```compile_fail
//! #[derive(psio::Key)]
//! struct Login {
//!     user: String,
//!     #[serde(skip_deserializing)]
//!     password: String,
//! }
//! ```
//! ```
//! #[derive(psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::Key)]
//! struct Login {
//!     user: String,
//!     #[serde(skip_serializing, skip_deserializing)]
//!     password: String,
//! }
//! assert_eq!(psio::to_json(&Login { user: "u".into(), password: "p".into() }), r#"{"user":"u"}"#);
//! ```
//!
//! A predicate other than `Option::is_none`:
//!
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! struct Point {
//!     #[serde(skip_serializing_if = "Vec::is_empty")]
//!     xs: Vec<u32>,
//! }
//! ```
//! ```
//! #[derive(psio::JsonText)]
//! struct Point {
//!     #[serde(skip_serializing_if = "Option::is_none")]
//!     x: Option<u32>,
//! }
//! assert_eq!(psio::to_json(&Point { x: None }), r#"{"x":null}"#);
//!
//! #[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::Key)]
//! struct Wire {
//!     #[serde(skip_serializing_if = "Vec::is_empty")]
//!     xs: Vec<u32>,
//! }
//! ```
//!
//! Variants:
//!
//! ```compile_fail
//! #[derive(psio::PsszPack)]
//! enum Shape {
//!     Circle(u32),
//!     #[serde(skip)]
//!     Hidden(u32),
//! }
//! ```
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! enum Shape {
//!     Circle(u32),
//!     #[serde(skip_serializing)]
//!     Hidden(u32),
//! }
//! ```
//! ```compile_fail
//! #[derive(psio::JsonText)]
//! #[repr(u8)]
//! enum Level {
//!     Low = 0,
//!     #[serde(other)]
//!     Unknown = 1,
//! }
//! ```
//! ```
//! #[derive(psio::JsonText, psio::PsszPack, psio::PsszUnpack, psio::Key)]
//! #[repr(u8)]
//! enum Level {
//!     Low = 0,
//!     #[psio(other)]
//!     Unknown(u8),
//! }
//! assert_eq!(psio::to_json(&Level::Unknown(9)), "9");
//!
//! #[derive(psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::Key)]
//! enum Shape {
//!     Circle(u32),
//!     #[serde(skip_serializing)]
//!     Hidden(u32),
//! }
//! ```
//!
//! A skipped field is a field of a struct, and the interchange derives have
//! none:
//!
//! ```compile_fail
//! #[derive(psio::PsszPack)]
//! enum Shape {
//!     Rect(u32, #[psio(skip)] u32),
//! }
//! ```
//! ```compile_fail
//! #[derive(psio::WitPack)]
//! struct Point {
//!     x: u32,
//!     #[psio(skip)]
//!     cache: u32,
//! }
//! ```
//! ```
//! #[derive(psio::WitPack, psio::PsszPack)]
//! struct Point {
//!     x: u32,
//! }
//! ```
//!
//! A skipped field has no view accessor:
//!
//! ```compile_fail
//! use psio::{PsszPack, PsszUnpack, PsszView};
//! #[derive(PsszPack, PsszUnpack, PsszView)]
//! struct Point {
//!     x: u32,
//!     #[psio(skip)]
//!     cache: u32,
//! }
//! let bytes = psio::to_pssz(&Point { x: 1, cache: 2 });
//! let view = psio::PsszView::<Point>::new(&bytes).unwrap();
//! view.cache();
//! ```
//! ```
//! use psio::{PsszPack, PsszUnpack, PsszView};
//! #[derive(PsszPack, PsszUnpack, PsszView)]
//! struct Point {
//!     x: u32,
//!     #[psio(skip)]
//!     cache: u32,
//! }
//! let bytes = psio::to_pssz(&Point { x: 1, cache: 2 });
//! let view = psio::PsszView::<Point>::new(&bytes).unwrap();
//! assert_eq!(view.x().get(), 1);
//! ```
