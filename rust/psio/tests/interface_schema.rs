//! The schema `SchemaBuilder` builds for an interface, for the records its
//! functions take and return, and for a schema that holds ordinary types
//! and an interface: node for node what C++ `psio::SchemaBuilder` builds
//! from the same declarations in the same order (the rules are on
//! `psio::pssz::schema_ir::SchemaBuilder`).
//!
//! `conformance/interface-vectors.json` pins one interface against C++
//! itself (`interface_conformance.rs`). The expected schemas here are
//! `interface_schema_oracle.txt`, written by `interface_schema_oracle.py`:
//! the Python implementation's port of the C++ builder, which regenerates
//! that fixture byte for byte, run over the declarations below. They are
//! not C++ output.

use psio::json::{self, Json};
use psio::pssz::dynamic;
use psio::pssz::schema_ir::{AnyType, AnyValue as V, Schema, SchemaBuilder};
use psio::pssz::RawString;
use std::collections::BTreeMap;

#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema, psio::JsonText)]
pub struct Point {
    pub x: i32,
    pub y: i32,
}

// C++: PSIO_REFLECT(Entry, attr(key, utf8 | max<12>), points,
//                   attr(tags, max<4> | element(utf8 | max<8>)), note, blob)
// over std::string key; std::vector<Point> points; std::vector<std::string>
// tags; std::optional<psio::utf8_string<>> note; std::vector<uint8_t> blob.
#[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema, psio::JsonText)]
pub struct Entry {
    #[psio(max = 12)]
    pub key: String,
    pub points: Vec<Point>,
    #[psio(max = 4, element(max = 8))]
    pub tags: Vec<String>,
    pub note: Option<String>,
    pub blob: Vec<u8>,
}

#[psio::interface(name = "ledger", package = "second", version = "1.2.0")]
pub trait Ledger {
    // Records as a parameter and as the result.
    fn put(&mut self, entry: Entry, at: Point) -> Entry;
    // A String with no bound (C++ psio::utf8_string<>), a bounded result.
    #[psio(returns(max = 24))]
    fn title(&self, id: u64, lang: String) -> String;
    // Positional, with both caps; a bounded list of records, bounded bytes.
    #[pssz(max_dynamic_data = 4096)]
    #[psio(max_fields = 6)]
    #[psio(positional)]
    fn bulk(&mut self, #[psio(max = 16)] entries: Vec<Entry>, #[psio(max = 64)] raw: Vec<u8>, flag: bool);
    // A bounded map with bounded keys; a bound two levels down.
    fn find(
        &self,
        #[psio(max = 3, keys(max = 8))] filter: BTreeMap<String, u32>,
        #[psio(element(element(max = 5)))] paths: Vec<Vec<RawString>>,
    ) -> Option<Entry>;
    // A bound through an Option; a bound on one element of the result.
    #[psio(returns(at(0, max = 10)))]
    fn pair(&self, #[psio(max = 20)] who: Option<RawString>) -> (String, i64);
    fn tick();
}

/// The oracle's schemas, by name: (Json text, PSSZ hex).
fn oracle() -> BTreeMap<String, (String, String)> {
    let text = include_str!("interface_schema_oracle.txt");
    let lines: Vec<&str> = text.lines().collect();
    assert_eq!(lines.len() % 3, 0);
    lines.chunks(3).map(|c| (c[0].to_string(), (c[1].to_string(), c[2].to_string()))).collect()
}
fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
#[track_caller]
fn same(ours: &Schema, name: &str) {
    let (text, bytes) = &oracle()[name];
    dynamic::validate_schema(ours).unwrap();
    assert_eq!(&json::try_encode::<Json, _>(ours).unwrap(), text, "{name}: the Json text");
    assert_eq!(&hex(&psio::to_pssz(ours)), bytes, "{name}: the PSSZ");
}

#[test]
fn an_interface_then_its_argument_lists() {
    let mut builder = SchemaBuilder::new();
    builder.insert_interface::<ledger::Tag>().unwrap();
    builder.insert_argument_lists::<ledger::Tag>().unwrap();
    let schema = builder.finish();
    same(&schema, "interface");

    // The record a function takes: under its own name, where the first
    // function that uses it is walked; its annotated members inline, after
    // the member's plain type (C++ std::string) was interned.
    assert_eq!(schema.types[0].name, "Entry");
    let V::Object(entry) = &schema.types[0].r#type.value else { panic!("an object") };
    assert_eq!(entry.members[0].name, "key");
    assert!(matches!(&entry.members[0].r#type.value, V::Custom(c) if c.id == "utf8"));
    // (the plain type of `key`: std::string, the "string" custom)
    assert!(matches!(&schema.types[1].r#type.value, V::Custom(c) if c.id == "string"));
    assert_eq!(schema.types[1].name, "@1");
    // An unbounded member is a reference.
    assert!(matches!(&entry.members[1].r#type.value, V::Type(_)));
    // Every argument list is an anonymous Object and a named alias of it.
    for method in ["put", "title", "bulk", "find", "pair", "tick"] {
        let at = schema.types.iter().position(|entry| entry.name == format!("ledger.{method}")).unwrap();
        let anonymous = &schema.types[at - 1];
        assert!(anonymous.name.starts_with('@'), "{method}");
        assert_eq!(*schema.types[at].r#type, AnyType::named(anonymous.name.clone()), "{method}");
        let V::Object(list) = &anonymous.r#type.value else { panic!("an object") };
        assert_eq!(list.attributes[0].name, "tuple");
        assert_eq!(list.attributes[0].value.as_deref(), Some(if method == "bulk" { "positional" } else { "named" }));
    }
}

#[test]
fn an_interface_alone() {
    let mut builder = SchemaBuilder::new();
    builder.insert_interface::<ledger::Tag>().unwrap();
    same(&builder.finish(), "bare");
}

/// A service schema: ordinary types, the interface, its argument lists,
/// more types. `@N` counts every type interned so far, in that order.
#[test]
fn ordinary_types_and_an_interface_in_one_schema() {
    let mut builder = SchemaBuilder::new();
    builder.insert_named::<Point>("Point").unwrap(); // its own name: no alias
    builder.insert_named::<Vec<Entry>>("Entries").unwrap();
    builder.insert_named::<u64>("Id").unwrap();
    builder.insert_named::<Entry>("Entry").unwrap();
    builder.insert_interface::<ledger::Tag>().unwrap();
    builder.insert_argument_lists::<ledger::Tag>().unwrap();
    builder.insert_named::<Vec<u8>>("Blob").unwrap();
    builder.insert_named::<(u8, u32)>("Limits").unwrap();
    let schema = builder.finish();
    same(&schema, "service");
    assert_eq!(schema.types.iter().filter(|entry| entry.name == "Point").count(), 1);
    // Point, its i32 (@1), then the list (@2) and its alias.
    assert_eq!(*schema.get("Entries").unwrap(), AnyType::named("@2"));

    // The order of the calls is part of the schema: the interface first is
    // another numbering of the same types.
    let mut other = SchemaBuilder::new();
    other.insert_interface::<ledger::Tag>().unwrap();
    other.insert_argument_lists::<ledger::Tag>().unwrap();
    other.insert_named::<Point>("Point").unwrap();
    other.insert_named::<Vec<Entry>>("Entries").unwrap();
    other.insert_named::<u64>("Id").unwrap();
    other.insert_named::<Entry>("Entry").unwrap();
    other.insert_named::<Vec<u8>>("Blob").unwrap();
    other.insert_named::<(u8, u32)>("Limits").unwrap();
    let other = other.finish();
    assert_ne!(other, schema);
    assert!(dynamic::equivalent_schema(&other, &schema, dynamic::Compare::Contract).ok);
}
