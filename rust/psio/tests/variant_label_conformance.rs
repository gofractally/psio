//! `conformance/variant-label-vectors.json` (described in
//! `conformance/variant-label-vectors.md`): the label of every alternative
//! of five variants, the Schema IR that names each alternative by its
//! label, and one value per alternative with its bytes and text.
//!
//! The variants are declared here as Rust enums. A Rust enum names its
//! alternatives, so the label is the variant's identifier; where the fixture
//! (a C++ `std::variant`, labelled by type name or by position) has another
//! label the twin states it with `#[psio(name = "...")]`. Every row is
//! checked on the typed path and on the schema-driven path, over the
//! fixture's schema and over the schema Rust builds.

use psio::json::{self, IJson, Json, JsonText};
use psio::pssz::dynamic::{self, Compare};
use psio::pssz::schema_ir::{schema_for, AnyType, AnyValue as V, Schema, ToPsszSchema};
use psio::pssz::{Pssz, PsszPack, PsszUnpack, PsszValidate, RawString};

macro_rules! twin {
    ($($item:item)*) => {$(
        #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::ToPsszSchema, psio::JsonText)]
        $item
    )*};
}

twin! {
    pub struct Circle { pub radius: u32 }
    pub struct Square { pub side: u32 }

    // Rule 1: the identifier is the label (here also the C++ type's name).
    pub enum Records { Circle(Circle), Square(Square) }

    // The C++ twin is labelled by type name: stated here.
    pub enum Scalars {
        #[psio(name = "bool")] Bool(bool),
        #[psio(name = "uint8")] U8(u8),
        #[psio(name = "int16")] I16(i16),
        #[psio(name = "uint32")] U32(u32),
        #[psio(name = "int64")] I64(i64),
        #[psio(name = "uint64")] U64(u64),
        #[psio(name = "float32")] F32(f32),
        #[psio(name = "double")] F64(f64),
        #[psio(name = "string")] Text(RawString),
    }

    pub enum Generic {
        #[psio(name = "uint32[]")] List(Vec<u32>),
        #[psio(name = "uint32?")] Maybe(Option<u32>),
        #[psio(name = "tuple_uint32_string")] Pair((u32, RawString)),
        #[psio(name = "uint16[2]")] Array([u16; 2]),
        #[psio(name = "Circle[]")] Circles(Vec<Circle>),
        #[psio(name = "uint8[]")] Bytes(Vec<u8>),
    }

    // C++ holds std::string twice: those two are labelled by position.
    pub enum Repeated {
        #[psio(name = "uint32")] Number(u32),
        #[psio(name = "1")] First(RawString),
        #[psio(name = "2")] Second(RawString),
        Circle(Circle),
    }

    // C++ states these labels with psio::variant_labels.
    pub enum Stated {
        #[psio(name = "circle")] Circle(Circle),
        #[psio(name = "square")] Square(Square),
        #[psio(name = "side")] Side(u32),
    }
}

#[derive(serde::Deserialize)]
struct VariantRow {
    name: String,
    labels: Vec<String>,
}
#[derive(serde::Deserialize)]
struct Row {
    name: String,
    r#type: String,
    index: usize,
    label: String,
    pssz: String,
    json: String,
    ijson: String,
}
#[derive(serde::Deserialize)]
struct Fixture {
    version: u32,
    schema_ir: String,
    schema_json: String,
    variants: Vec<VariantRow>,
    vectors: Vec<Row>,
}

fn fixture() -> Fixture {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../conformance/variant-label-vectors.json");
    serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap()
}
fn unhex(hex: &str) -> Vec<u8> {
    (0..hex.len()).step_by(2).map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap()).collect()
}

/// The labels of `T`, the equivalence of its schema node with the fixture's,
/// and every row of the fixture for it.
fn check<T>(fx: &Fixture, theirs: &Schema, name: &str) -> usize
where
    T: ToPsszSchema + PsszPack<Pssz> + PsszUnpack<Pssz> + PsszValidate<Pssz> + JsonText + PartialEq + std::fmt::Debug,
{
    let (ours, root) = schema_for::<T>().unwrap();
    dynamic::validate_schema(&ours).unwrap();
    let their_root = AnyType::named(name);

    // The schema names each alternative by its label and says nothing else
    // about it.
    let labels = &fx.variants.iter().find(|v| v.name == name).unwrap().labels;
    for (schema, node) in [(&ours, &root), (theirs, &their_root)] {
        let V::Variant(variant) = &schema.resolve(node).unwrap().value else { panic!("{name}: not a variant") };
        assert_eq!(&variant.members.iter().map(|m| m.name.clone()).collect::<Vec<_>>(), labels, "{name}");
        assert!(variant.members.iter().all(|m| m.attributes.is_empty()), "{name}");
    }
    // One schema node in both languages: equivalent as a contract (names,
    // labels, text forms), not only on the wire.
    let same = dynamic::equivalent(&ours, &root, theirs, &their_root, Compare::Contract);
    assert!(same.ok, "{name}: {same:?}");

    let mut rows = 0;
    for row in fx.vectors.iter().filter(|row| row.r#type == name) {
        rows += 1;
        let bytes = unhex(&row.pssz);
        assert_eq!(labels[row.index], row.label, "{}", row.name);
        assert_eq!(bytes[0] as usize, row.index, "{}", row.name);
        // Typed.
        psio::validate_pssz::<T>(&bytes).unwrap_or_else(|e| panic!("{}: {e}", row.name));
        let value = psio::from_pssz::<T>(&bytes).unwrap();
        assert_eq!(psio::to_pssz(&value), bytes, "{}", row.name);
        assert_eq!(json::try_encode::<Json, _>(&value).unwrap(), row.json, "{}", row.name);
        assert_eq!(json::try_encode::<IJson, _>(&value).unwrap(), row.ijson, "{}", row.name);
        assert_eq!(json::decode::<Json, T>(&row.json).as_ref(), Ok(&value), "{}", row.name);
        assert_eq!(json::decode::<IJson, T>(&row.ijson).as_ref(), Ok(&value), "{}", row.name);
        // Schema-driven, over the fixture's schema and over ours.
        for (schema, node) in [(theirs, &their_root), (&ours, &root)] {
            assert_eq!(dynamic::validate(schema, node, &bytes), Ok(false), "{}", row.name);
            let (text, _) = dynamic::json::pssz_to_json::<Json>(schema, node, &bytes).unwrap();
            assert_eq!(text, row.json, "{}", row.name);
            let (text, _) = dynamic::json::pssz_to_json::<IJson>(schema, node, &bytes).unwrap();
            assert_eq!(text, row.ijson, "{}", row.name);
            assert_eq!(
                dynamic::json::json_to_pssz::<Json>(schema, node, row.json.as_bytes()).as_deref(),
                Ok(&bytes[..]),
                "{}",
                row.name
            );
            assert_eq!(
                dynamic::json::json_to_pssz::<IJson>(schema, node, row.ijson.as_bytes()).as_deref(),
                Ok(&bytes[..]),
                "{}",
                row.name
            );
        }
    }
    rows
}

#[test]
fn every_variant_has_the_fixture_labels_and_rows() {
    let fx = fixture();
    assert_eq!(fx.version, 1);
    let theirs: Schema = psio::from_pssz(&unhex(&fx.schema_ir)).unwrap();
    assert_eq!(json::decode::<Json, Schema>(&fx.schema_json).unwrap(), theirs);
    dynamic::validate_schema(&theirs).unwrap();

    let rows = check::<Records>(&fx, &theirs, "Records")
        + check::<Scalars>(&fx, &theirs, "Scalars")
        + check::<Generic>(&fx, &theirs, "Generic")
        + check::<Repeated>(&fx, &theirs, "Repeated")
        + check::<Stated>(&fx, &theirs, "Stated");
    // Nothing skipped.
    assert_eq!(fx.variants.len(), 5);
    assert_eq!(rows, fx.vectors.len());
    assert_eq!(rows, 25);
}

/// A label is the alternative's name in the schema: the variant's
/// identifier, or what `#[psio(name)]` states. There is no second name.
#[test]
fn a_stated_label_is_the_schema_name_and_the_text_key() {
    let (schema, root) = schema_for::<Stated>().unwrap();
    let V::Variant(variant) = &schema.resolve(&root).unwrap().value else { panic!() };
    assert_eq!(variant.members[2].name, "side");
    assert!(variant.members[2].attributes.is_empty());
    assert_eq!(json::try_encode::<IJson, _>(&Stated::Side(5)).unwrap(), r#"{"side":5}"#);
    assert_eq!(json::decode::<IJson, Stated>(r#"{"side":5}"#), Ok(Stated::Side(5)));
    // The identifier is not a second key.
    assert!(json::decode::<IJson, Stated>(r#"{"Side":5}"#).is_err());

    // A changed label is a contract change, not a wire change.
    twin! {
        pub enum Renamed {
            #[psio(name = "circle")] Circle(Circle),
            #[psio(name = "square")] Square(Square),
            Side(u32),
        }
    }
    let (other, other_root) = schema_for::<Renamed>().unwrap();
    assert!(dynamic::equivalent(&schema, &root, &other, &other_root, Compare::Wire).ok);
    let contract = dynamic::equivalent(&schema, &root, &other, &other_root, Compare::Contract);
    assert_eq!((contract.ok, contract.code), (false, "alternative-renamed"));
}
