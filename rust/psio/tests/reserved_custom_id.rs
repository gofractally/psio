//! A custom type cannot claim an id the Schema IR reserves for PSIO's own
//! types (RG4). The derives refuse it at compile time (the `compile_fail`
//! examples of `psio::pssz::schema_ir::reserved_custom_id`); the schema
//! builder refuses a hand-written impl.

use psio::pssz::schema_ir::{
    reserved_custom_id, schema_for, AnyType, AnyValue, Custom, Int, SchemaBuilder, ToPsszSchema,
};
use psio::pssz::PsszResult;
use psio::{PsszPack, PsszUnpack};

#[test]
fn the_reserved_ids() {
    for id in [
        "bool", "char", "string", "utf8", "hex", "bitvector", "bitlist", "map", "set", "unknown", "box", "json",
        "time-point", "time-point-seconds", "time-point-micros", "time-point-1/60", "time-point-anything",
        "duration", "duration-nanos", "monotonic", "monotonic-millis",
    ] {
        assert!(reserved_custom_id(id), "{id}");
    }
    for id in ["AccountNumber", "Memo", "time", "timepoint", "durations", "time-pointer", "Bool", "boxed", ""] {
        assert!(!reserved_custom_id(id), "{id}");
    }
}

/// A hand-written schema that claims a time id over a record.
struct Claims;
impl ToPsszSchema for Claims {
    fn schema_name() -> Option<String> {
        Some("Claims".into())
    }
    fn custom_id() -> Option<String> {
        Some("time-point-seconds".into())
    }
    fn build_schema(_: &mut SchemaBuilder) -> PsszResult<AnyType> {
        Ok(AnyType::new(AnyValue::Custom(Custom {
            r#type: Box::new(AnyType::new(AnyValue::Int(Int { bits: 64, isSigned: true }))),
            id: "time-point-seconds".into(),
        })))
    }
}

#[derive(PsszPack, PsszUnpack, psio::ToPsszSchema)]
struct Holder {
    #[allow(dead_code)]
    id: u32,
}

#[test]
fn the_builder_refuses_a_hand_written_claim() {
    let error = schema_for::<Claims>().unwrap_err();
    assert!(error.message().contains("reserved"), "{error}");
    // Nothing of the refused type is left in the builder.
    let mut builder = SchemaBuilder::new();
    assert!(builder.insert::<Claims>().is_err());
    builder.insert::<Holder>().unwrap();
    let schema = builder.finish();
    assert!(schema.get("Claims").is_none());
    assert!(schema.get("Holder").is_some());
}

/// A custom id of the type's own is what the attribute is for, and PSIO's
/// own time types keep their ids.
#[derive(PsszPack, PsszUnpack, psio::ToPsszSchema)]
#[psio(definition_will_not_change, custom = "TimePointSec")]
struct TimePointSec {
    #[allow(dead_code)]
    seconds: i64,
}

#[test]
fn other_ids_and_psios_own_types_are_unaffected() {
    let (schema, root) = schema_for::<TimePointSec>().unwrap();
    let AnyValue::Custom(custom) = &schema.resolve(&root).unwrap().value else { panic!("custom") };
    assert_eq!(custom.id, "TimePointSec");
    let (schema, root) = schema_for::<psio::time::TimePoint<psio::time::Seconds>>().unwrap();
    let AnyValue::Custom(custom) = &schema.resolve(&root).unwrap().value else { panic!("custom") };
    assert_eq!(custom.id, "time-point-seconds");
    schema_for::<(bool, String, Vec<u8>, Box<u32>, std::collections::BTreeMap<u8, u8>)>().unwrap();
}
